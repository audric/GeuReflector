/**
@file	 Reflector.cpp
@brief   The main reflector class
@author  Tobias Blomberg / SM0SVX
@date	 2017-02-11

\verbatim
SvxReflector - An audio reflector for connecting SvxLink Servers
Copyright (C) 2003-2026 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/

/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <cassert>
#include <sstream>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <regex>
#include <dirent.h>   // for listing directories (list certs)
#include <sys/stat.h> // for checking if a directory exists (list certs)


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncConfig.h>
#include <AsyncTcpServer.h>
#include <AsyncDigest.h>
#include <AsyncSslCertSigningReq.h>
#include <AsyncEncryptedUdpSocket.h>
#include <AsyncApplication.h>
#include <AsyncPty.h>

#include <common.h>
#include <config.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "Reflector.h"
#include "ReflectorClient.h"
#include "TGHandler.h"
#include "TrunkLink.h"
#include <version/SVXREFLECTOR.h>


/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;



/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/

#define RENEW_AFTER 2.0/3.0


/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local functions
 *
 ****************************************************************************/

namespace {
  bool isCallsignPathSafe(const std::string& s)
  {
    if (s.empty()) return false;
    if (s.find('/') != std::string::npos) return false;
    if (s.find('\0') != std::string::npos) return false;
    if (s.find("..") != std::string::npos) return false;
    return true;
  }

  //void splitFilename(const std::string& filename, std::string& dirname,
  //    std::string& basename)
  //{
  //  std::string ext;
  //  basename = filename;

  //  size_t basenamepos = filename.find_last_of('/');
  //  if (basenamepos != string::npos)
  //  {
  //    if (basenamepos + 1 < filename.size())
  //    {
  //      basename = filename.substr(basenamepos + 1);
  //    }
  //    dirname = filename.substr(0, basenamepos + 1);
  //  }

  //  size_t extpos = basename.find_last_of('.');
  //  if (extpos != string::npos)
  //  {
  //    if (extpos+1 < basename.size())
  //    ext = basename.substr(extpos+1);
  //    basename.erase(extpos);
  //  }
  //}

  bool ensureDirectoryExist(const std::string& path)
  {
    std::vector<std::string> parts;
    SvxLink::splitStr(parts, path, "/");
    std::string dirname;
    if (path[0] == '/')
    {
      dirname = "/";
    }
    else if (path[0] != '.')
    {
      dirname = "./";
    }
    if (path.back() != '/')
    {
      parts.erase(std::prev(parts.end()));
    }
    for (const auto& part : parts)
    {
      dirname += part + "/";
      if (access(dirname.c_str(), F_OK) != 0)
      {
        std::cout << "Create directory '" << dirname << "'" << std::endl;
        if (mkdir(dirname.c_str(), 0777) != 0)
        {
          std::cerr << "*** ERROR: Could not create directory '"
                    << dirname << "'" << std::endl;
          return false;
        }
      }
    }
    return true;
  } /* ensureDirectoryExist */


  void startCertRenewTimer(const Async::SslX509& cert, Async::AtTimer& timer)
  {
    int days=0, seconds=0;
    cert.validityTime(days, seconds);
    time_t renew_time = cert.notBefore() +
        (static_cast<time_t>(days)*24*3600 + seconds)*RENEW_AFTER;
    timer.setTimeout(renew_time);
    timer.setExpireOffset(10000);
    timer.start();
  } /* startCertRenewTimer */
};


/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/

namespace {
  ReflectorClient::ProtoVerRangeFilter v1_client_filter(
      ProtoVer(1, 0), ProtoVer(1, 999));
  //ReflectorClient::ProtoVerRangeFilter v2_client_filter(
  //    ProtoVer(2, 0), ProtoVer(2, 999));
  ReflectorClient::ProtoVerLargerOrEqualFilter ge_v2_client_filter(
      ProtoVer(2, 0));
};


/****************************************************************************
 *
 * Public static functions
 *
 ****************************************************************************/

time_t Reflector::timeToRenewCert(const Async::SslX509& cert)
{
  if (cert.isNull())
  {
    return 0;
  }

  int days=0, seconds=0;
  cert.validityTime(days, seconds);
  time_t renew_time = cert.notBefore() +
    (static_cast<time_t>(days)*24*3600 + seconds)*RENEW_AFTER;
  return renew_time;
} /* Reflector::timeToRenewCert */


/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/

Reflector::Reflector(void)
  : m_srv(0), m_udp_sock(0), m_tg_for_v1_clients(1), m_random_qsy_lo(0),
    m_random_qsy_hi(0), m_random_qsy_tg(0), m_http_server(0), m_cmd_pty(0),
    m_keys_dir("private/"), m_pending_csrs_dir("pending_csrs/"),
    m_csrs_dir("csrs/"), m_certs_dir("certs/"), m_pki_dir("pki/"),
    m_sat_cleanup_timer(0, Async::Timer::TYPE_ONESHOT)
{
  TGHandler::instance()->talkerUpdated.connect(
      mem_fun(*this, &Reflector::onTalkerUpdated));
  TGHandler::instance()->requestAutoQsy.connect(
      mem_fun(*this, &Reflector::onRequestAutoQsy));
  TGHandler::instance()->trunkTalkerUpdated.connect(
      mem_fun(*this, &Reflector::onTrunkTalkerUpdated));
  m_renew_cert_timer.expired.connect(
      [&](Async::AtTimer*)
      {
        if (!loadServerCertificateFiles())
        {
          std::cerr << "*** WARNING: Failed to renew server certificate"
                    << std::endl;
        }
      });
  m_renew_issue_ca_cert_timer.expired.connect(
      [&](Async::AtTimer*)
      {
        if (!loadSigningCAFiles())
        {
          std::cerr << "*** WARNING: Failed to renew issuing CA certificate"
                    << std::endl;
        }
      });
  m_sat_cleanup_timer.setEnable(false);
  m_sat_cleanup_timer.expired.connect(
      sigc::mem_fun(*this, &Reflector::processSatelliteCleanup));
  m_status["nodes"] = Json::Value(Json::objectValue);
} /* Reflector::Reflector */


Reflector::~Reflector(void)
{
  delete m_http_server;
  m_http_server = 0;
  delete m_udp_sock;
  m_udp_sock = 0;
  delete m_srv;
  m_srv = 0;
  delete m_cmd_pty;
  m_cmd_pty = 0;
  for (auto* link : m_trunk_links)
  {
    delete link;
  }
  m_trunk_links.clear();
  for (auto& kv : m_trunk_pending_cons)
  {
    delete kv.second;
  }
  m_trunk_pending_cons.clear();
  m_trunk_inbound_map.clear();
  delete m_trunk_srv;
  m_trunk_srv = nullptr;
  for (auto& kv : m_satellite_con_map)
  {
    delete kv.second;
  }
  m_satellite_con_map.clear();
  delete m_sat_srv;
  m_sat_srv = nullptr;
  delete m_satellite_client;
  m_satellite_client = nullptr;
  m_client_con_map.clear();
  ReflectorClient::cleanup();
  delete TGHandler::instance();
} /* Reflector::~Reflector */


bool Reflector::initialize(Async::Config &cfg)
{
  m_cfg = &cfg;
  TGHandler::instance()->setConfig(m_cfg);

  std::string listen_port("5300");
  cfg.getValue("GLOBAL", "LISTEN_PORT", listen_port);
  m_srv = new TcpServer<FramedTcpConnection>(listen_port);
  m_srv->clientConnected.connect(
      mem_fun(*this, &Reflector::clientConnected));
  m_srv->clientDisconnected.connect(
      mem_fun(*this, &Reflector::clientDisconnected));

  if (!loadCertificateFiles())
  {
    return false;
  }

  m_srv->setSslContext(m_ssl_ctx);

  uint16_t udp_listen_port = 5300;
  cfg.getValue("GLOBAL", "LISTEN_PORT", udp_listen_port);
  m_udp_sock = new Async::EncryptedUdpSocket(udp_listen_port);
  const char* err = "unknown reason";
  if ((err="bad allocation",          (m_udp_sock == 0)) ||
      (err="initialization failure",  !m_udp_sock->initOk()) ||
      (err="unsupported cipher",      !m_udp_sock->setCipher(UdpCipher::NAME)))
  {
    std::cerr << "*** ERROR: Could not initialize UDP socket due to "
              << err << std::endl;
    return false;
  }
  m_udp_sock->setCipherAADLength(UdpCipher::AADLEN);
  m_udp_sock->setTagLength(UdpCipher::TAGLEN);
  m_udp_sock->cipherDataReceived.connect(
      mem_fun(*this, &Reflector::udpCipherDataReceived));
  m_udp_sock->dataReceived.connect(
      mem_fun(*this, &Reflector::udpDatagramReceived));

  unsigned sql_timeout = 0;
  cfg.getValue("GLOBAL", "SQL_TIMEOUT", sql_timeout);
  TGHandler::instance()->setSqlTimeout(sql_timeout);

  unsigned sql_timeout_blocktime = 60;
  cfg.getValue("GLOBAL", "SQL_TIMEOUT_BLOCKTIME", sql_timeout_blocktime);
  TGHandler::instance()->setSqlTimeoutBlocktime(sql_timeout_blocktime);

  m_cfg->getValue("GLOBAL", "TG_FOR_V1_CLIENTS", m_tg_for_v1_clients);

  SvxLink::SepPair<uint32_t, uint32_t> random_qsy_range;
  if (m_cfg->getValue("GLOBAL", "RANDOM_QSY_RANGE", random_qsy_range))
  {
    m_random_qsy_lo = random_qsy_range.first;
    m_random_qsy_hi = m_random_qsy_lo + random_qsy_range.second-1;
    if ((m_random_qsy_lo < 1) || (m_random_qsy_hi < m_random_qsy_lo))
    {
      cout << "*** WARNING: Illegal RANDOM_QSY_RANGE specified. Ignored."
           << endl;
      m_random_qsy_hi = m_random_qsy_lo = 0;
    }
    m_random_qsy_tg = m_random_qsy_hi;
  }

  std::string http_srv_port;
  if (m_cfg->getValue("GLOBAL", "HTTP_SRV_PORT", http_srv_port))
  {
    m_http_server = new Async::TcpServer<Async::HttpServerConnection>(http_srv_port);
    m_http_server->clientConnected.connect(
        sigc::mem_fun(*this, &Reflector::httpClientConnected));
    m_http_server->clientDisconnected.connect(
        sigc::mem_fun(*this, &Reflector::httpClientDisconnected));
  }

    // Path for command PTY
  string pty_path;
  m_cfg->getValue("GLOBAL", "COMMAND_PTY", pty_path);
  if (!pty_path.empty())
  {
    m_cmd_pty = new Pty(pty_path);
    if ((m_cmd_pty == nullptr) || !m_cmd_pty->open())
    {
      std::cerr << "*** ERROR: Could not open command PTY '" << pty_path
                << "' as specified in configuration variable "
                   "GLOBAL/COMMAND_PTY" << std::endl;
      return false;
    }
    m_cmd_pty->setLineBuffered(true);
    m_cmd_pty->dataReceived.connect(
        mem_fun(*this, &Reflector::ctrlPtyDataReceived));
  }

  m_cfg->getValue("GLOBAL", "ACCEPT_CERT_EMAIL", m_accept_cert_email);

  m_cfg->valueUpdated.connect(sigc::mem_fun(*this, &Reflector::cfgUpdated));

  // Parse CLUSTER_TGS — comma-separated list of TG numbers broadcast to all peers
  std::string cluster_tgs_str;
  if (cfg.getValue("GLOBAL", "CLUSTER_TGS", cluster_tgs_str))
  {
    std::istringstream ctss(cluster_tgs_str);
    std::string token;
    while (std::getline(ctss, token, ','))
    {
      token.erase(0, token.find_first_not_of(" \t"));
      token.erase(token.find_last_not_of(" \t") + 1);
      if (!token.empty())
      {
        uint32_t tg = static_cast<uint32_t>(std::stoul(token));
        m_cluster_tgs.insert(tg);
      }
    }
    if (!m_cluster_tgs.empty())
    {
      std::cout << "Cluster TGs:";
      for (uint32_t tg : m_cluster_tgs)
      {
        std::cout << " " << tg;
      }
      std::cout << std::endl;
    }
  }

  // Satellite mode: connect to parent instead of joining the trunk mesh
  std::string satellite_of;
  if (cfg.getValue("GLOBAL", "SATELLITE_OF", satellite_of) &&
      !satellite_of.empty())
  {
    m_is_satellite = true;
    m_satellite_client = new SatelliteClient(this, cfg);
    if (!m_satellite_client->initialize())
    {
      std::cerr << "*** ERROR: Failed to initialize satellite connection"
                << std::endl;
      return false;
    }
    // Satellites don't participate in the trunk mesh
    return true;
  }

  // TRUNK_DEBUG — verbose logging for diagnosing trunk connection issues
  std::string trunk_debug_str;
  if (cfg.getValue("GLOBAL", "TRUNK_DEBUG", trunk_debug_str))
  {
    m_trunk_debug = (trunk_debug_str == "1" || trunk_debug_str == "true"
                     || trunk_debug_str == "yes");
    if (m_trunk_debug)
    {
      std::cout << "Trunk debug logging enabled" << std::endl;
    }
  }

  initTrunkLinks();
  initTrunkServer();
  initSatelliteServer();

  return true;
} /* Reflector::initialize */


void Reflector::nodeList(std::vector<std::string>& nodes) const
{
  nodes.clear();
  for (const auto& item : m_client_con_map)
  {
    const std::string& callsign = item.second->callsign();
    if (!callsign.empty())
    {
      nodes.push_back(callsign);
    }
  }
} /* Reflector::nodeList */


void Reflector::broadcastMsg(const ReflectorMsg& msg,
                             const ReflectorClient::Filter& filter)
{
  for (const auto& item : m_client_con_map)
  {
    ReflectorClient *client = item.second;
    if (filter(client) &&
        (client->conState() == ReflectorClient::STATE_CONNECTED))
    {
      client->sendMsg(msg);
    }
  }
} /* Reflector::broadcastMsg */


bool Reflector::sendUdpDatagram(ReflectorClient *client,
    const ReflectorUdpMsg& msg)
{
  auto udp_addr = client->remoteUdpHost();
  auto udp_port = client->remoteUdpPort();
  if (client->protoVer() >= ProtoVer(3, 0))
  {
    ReflectorUdpMsg header(msg.type());
    ostringstream ss;
    assert(header.pack(ss) && msg.pack(ss));

    m_udp_sock->setCipherIV(client->udpCipherIV());
    m_udp_sock->setCipherKey(client->udpCipherKey());
    UdpCipher::AAD aad{client->udpCipherIVCntrNext()};
    std::stringstream aadss;
    if (!aad.pack(aadss))
    {
      std::cout << "*** WARNING: Packing associated data failed for UDP "
                   "datagram to " << udp_addr << ":" << udp_port << std::endl;
      return false;
    }
    return m_udp_sock->write(udp_addr, udp_port,
                             aadss.str().data(), aadss.str().size(),
                             ss.str().data(), ss.str().size());
  }
  else
  {
    ReflectorUdpMsgV2 header(msg.type(), client->clientId(),
        client->udpCipherIVCntrNext() & 0xffff);
    ostringstream ss;
    assert(header.pack(ss) && msg.pack(ss));
    return m_udp_sock->UdpSocket::write(
        udp_addr, udp_port,
        ss.str().data(), ss.str().size());
  }
} /* Reflector::sendUdpDatagram */


void Reflector::broadcastUdpMsg(const ReflectorUdpMsg& msg,
                                const ReflectorClient::Filter& filter)
{
  for (const auto& item : m_client_con_map)
  {
    ReflectorClient *client = item.second;
    if (filter(client) &&
        (client->conState() == ReflectorClient::STATE_CONNECTED))
    {
      client->sendUdpMsg(msg);
    }
  }
} /* Reflector::broadcastUdpMsg */


void Reflector::requestQsy(ReflectorClient *client, uint32_t tg)
{
  uint32_t current_tg = TGHandler::instance()->TGForClient(client);
  if (current_tg == 0)
  {
    std::cout << client->callsign()
              << ": Cannot request QSY from TG #0" << std::endl;
    return;
  }

  if (tg == 0)
  {
    tg = nextRandomQsyTg();
    if (tg == 0) { return; }
  }

  cout << client->callsign() << ": Requesting QSY from TG #"
       << current_tg << " to TG #" << tg << endl;

  broadcastMsg(MsgRequestQsy(tg),
      ReflectorClient::mkAndFilter(
        ge_v2_client_filter,
        ReflectorClient::TgFilter(current_tg)));
} /* Reflector::requestQsy */


Async::SslCertSigningReq
Reflector::loadClientPendingCsr(const std::string& callsign)
{
  Async::SslCertSigningReq csr;
  if (!isCallsignPathSafe(callsign)) return csr;
  (void)csr.readPemFile(m_pending_csrs_dir + "/" + callsign + ".csr");
  return csr;
} /* Reflector::loadClientPendingCsr */


Async::SslCertSigningReq
Reflector::loadClientCsr(const std::string& callsign)
{
  Async::SslCertSigningReq csr;
  if (!isCallsignPathSafe(callsign)) return csr;
  (void)csr.readPemFile(m_csrs_dir + "/" + callsign + ".csr");
  return csr;
} /* Reflector::loadClientPendingCsr */


bool Reflector::renewedClientCert(Async::SslX509& cert)
{
  if (cert.isNull())
  {
    return false;
  }

  std::string callsign(cert.commonName());
  Async::SslX509 new_cert = loadClientCertificate(callsign);
  if (!new_cert.isNull() &&
      ((new_cert.publicKey() != cert.publicKey()) ||
       (timeToRenewCert(new_cert) <= std::time(NULL))))
  {
    return signClientCert(cert, "CRT_RENEWED");
  }
  cert = std::move(new_cert);
  return !cert.isNull();
} /* Reflector::renewedClientCert */


bool Reflector::signClientCert(Async::SslX509& cert, const std::string& ca_op)
{
  //std::cout << "### Reflector::signClientCert" << std::endl;

  cert.setSerialNumber();
  cert.setIssuerName(m_issue_ca_cert.subjectName());
  cert.setValidityTime(CERT_VALIDITY_DAYS, CERT_VALIDITY_OFFSET_DAYS);
  auto cn = cert.commonName();
  if (!cert.sign(m_issue_ca_pkey))
  {
    std::cerr << "*** ERROR: Certificate signing failed for client "
              << cn << std::endl;
    return false;
  }
  auto crtfile = m_certs_dir + "/" + cn + ".crt";
  if (cert.writePemFile(crtfile) && m_issue_ca_cert.appendPemFile(crtfile))
  {
    runCAHook({
        { "CA_OP",      ca_op },
        { "CA_CRT_PEM", cert.pem() }
      });
  }
  else
  {
    std::cerr << "*** WARNING: Failed to write client certificate file '"
              << crtfile << "'" << std::endl;
  }
  return true;
} /* Reflector::signClientCert */


Async::SslX509 Reflector::signClientCsr(const std::string& cn)
{
  //std::cout << "### Reflector::signClientCsr" << std::endl;

  Async::SslX509 cert(nullptr);

  auto req = loadClientPendingCsr(cn);
  if (req.isNull())
  {
    std::cerr << "*** ERROR: Cannot find CSR to sign '" << req.filePath()
              << "'" << std::endl;
    return cert;
  }

  cert.clear();
  cert.setVersion(Async::SslX509::VERSION_3);
  cert.setSubjectName(req.subjectName());
  const Async::SslX509Extensions exts(req.extensions());
  Async::SslX509Extensions cert_exts;
  cert_exts.addBasicConstraints("critical, CA:FALSE");
  cert_exts.addKeyUsage(
      "critical, digitalSignature, keyEncipherment, keyAgreement");
  cert_exts.addExtKeyUsage("clientAuth");
  Async::SslX509ExtSubjectAltName san(exts.subjectAltName());
  cert_exts.addExtension(san);
  cert.addExtensions(cert_exts);
  Async::SslKeypair csr_pkey(req.publicKey());
  cert.setPublicKey(csr_pkey);

  if (!signClientCert(cert, "CSR_SIGNED"))
  {
    cert.set(nullptr);
  }

  std::string csr_path = m_csrs_dir + "/" + cn + ".csr";
  if (rename(req.filePath().c_str(), csr_path.c_str()) != 0)
  {
    auto errstr = SvxLink::strError(errno);
    std::cerr << "*** WARNING: Failed to move signed CSR from '"
              << req.filePath() << "' to '" << csr_path << "': "
              << errstr << std::endl;
  }

  auto client = ReflectorClient::lookup(cn);
  if ((client != nullptr) && !cert.isNull())
  {
    client->certificateUpdated(cert);
  }

  return cert;
} /* Reflector::signClientCsr */


Async::SslX509 Reflector::loadClientCertificate(const std::string& callsign)
{
  Async::SslX509 cert;
  if (!isCallsignPathSafe(callsign)) return nullptr;
  if (!cert.readPemFile(m_certs_dir + "/" + callsign + ".crt") ||
      cert.isNull() ||
      //!cert.verify(m_issue_ca_pkey) ||
      !cert.timeIsWithinRange())
  {
    return nullptr;
  }
  return cert;
} /* Reflector::loadClientCertificate */


std::string Reflector::clientCertPem(const std::string& callsign) const
{
  if (!isCallsignPathSafe(callsign)) return std::string();
  std::string crtfile(m_certs_dir + "/" + callsign + ".crt");
  std::ifstream ifs(crtfile);
  if (!ifs.good())
  {
    return std::string();
  }
  return std::string(std::istreambuf_iterator<char>{ifs}, {});
} /* Reflector::clientCertPem */


std::string Reflector::caBundlePem(void) const
{
  std::ifstream ifs(m_ca_bundle_file);
  if (ifs.good())
  {
    return std::string(std::istreambuf_iterator<char>{ifs}, {});
  }
  return std::string();
} /* Reflector::caBundlePem */


std::string Reflector::issuingCertPem(void) const
{
  return m_issue_ca_cert.pem();
} /* Reflector::issuingCertPem */


bool Reflector::callsignOk(const std::string& callsign, bool verbose) const
{
    // Empty check
  if (callsign.empty())
  {
    if (verbose)
    {
      std::cout << "*** WARNING: The callsign is empty" << std::endl;
    }
    return false;
  }

  try
  {
      // Accept check
    std::string accept_cs_re_str;
    if (!m_cfg->getValue("GLOBAL", "ACCEPT_CALLSIGN", accept_cs_re_str) ||
        accept_cs_re_str.empty())
    {
      accept_cs_re_str =
        "[A-Z0-9][A-Z]{0,2}\\d[A-Z0-9]{0,3}[A-Z](?:-[A-Z0-9]{1,3})?";
    }
    const std::regex accept_callsign_re(accept_cs_re_str);
    if (!std::regex_match(callsign, accept_callsign_re))
    {
      if (verbose)
      {
        std::cerr << "*** WARNING: The callsign '" << callsign
                  << "' is not accepted by configuration (ACCEPT_CALLSIGN)"
                  << std::endl;
      }
      return false;
    }

      // Reject check
    std::string reject_cs_re_str;
    m_cfg->getValue("GLOBAL", "REJECT_CALLSIGN", reject_cs_re_str);
    if (!reject_cs_re_str.empty())
    {
      const std::regex reject_callsign_re(reject_cs_re_str);
      if (std::regex_match(callsign, reject_callsign_re))
      {
        if (verbose)
        {
          std::cerr << "*** WARNING: The callsign '" << callsign
                    << "' has been rejected by configuration (REJECT_CALLSIGN)."
                    << std::endl;
        }
        return false;
      }
    }
  }
  catch (std::regex_error& e)
  {
    std::cerr << "*** WARNING: Regular expression parsing error in "
              << "ACCEPT_CALLSIGN/REJECT_CALLSIGN: " << e.what() << std::endl;
    return false;
  }

  return true;
} /* Reflector::callsignOk */


bool Reflector::emailOk(const std::string& email) const
{
  if (m_accept_cert_email.empty())
  {
    return true;
  }
  try
  {
    return std::regex_match(email, std::regex(m_accept_cert_email));
  }
  catch (std::regex_error& e)
  {
    std::cerr << "*** WARNING: Regular expression parsing error in "
              << "ACCEPT_CERT_EMAIL: " << e.what() << std::endl;
    return false;
  }
} /* Reflector::emailOk */


bool Reflector::reqEmailOk(const Async::SslCertSigningReq& req) const
{
  if (req.isNull())
  {
    return false;
  }

  const auto san = req.extensions().subjectAltName();
  if (san.isNull())
  {
    return emailOk("");
  }

  size_t email_cnt = 0;
  bool email_ok = true;
  san.forEach(
      [&](int type, std::string value)
      {
        email_cnt += 1;
        email_ok &= emailOk(value);
      },
      GEN_EMAIL);
  email_ok &= (email_cnt > 0) || emailOk("");
  return email_ok;
} /* Reflector::reqEmailOk */


std::string Reflector::checkCsr(const Async::SslCertSigningReq& req)
{
  if (!callsignOk(req.commonName()))
  {
    return std::string("Certificate signing request with invalid callsign '") +
           req.commonName() + "'";
  }
  if (!reqEmailOk(req))
  {
    return std::string(
             "Certificate signing request with no or invalid CERT_EMAIL"
           );
  }
  return "";
} /* Reflector::checkCsr */


Async::SslX509 Reflector::csrReceived(Async::SslCertSigningReq& req)
{
  if (req.isNull())
  {
    return nullptr;
  }

  std::string callsign(req.commonName());
  if (!callsignOk(callsign))
  {
    std::cerr << "*** WARNING: The CSR CN (callsign) check failed"
              << std::endl;
    return nullptr;
  }

  std::string csr_path(m_csrs_dir + "/" + callsign + ".csr");
  Async::SslCertSigningReq csr;
  if (!csr.readPemFile(csr_path))
  {
    csr.set(nullptr);
  }

  if (!csr.isNull() && (req.publicKey() != csr.publicKey()))
  {
    std::cerr << "*** WARNING: The received CSR with callsign '"
              << callsign << "' has a different public key "
                 "than the current CSR. That may be a sign of someone "
                 "trying to hijack a callsign or the owner of the "
                 "callsign has generated a new private/public key pair."
              << std::endl;
    return nullptr;
  }

  Async::SslX509 cert = loadClientCertificate(callsign);
  if (!cert.isNull() &&
      ((cert.publicKey() != req.publicKey()) ||
       (timeToRenewCert(cert) <= std::time(NULL))))
  {
    cert.set(nullptr);
  }

  const std::string pending_csr_path(
      m_pending_csrs_dir + "/" + callsign + ".csr");
  Async::SslCertSigningReq pending_csr;
  if ((
        csr.isNull() ||
        (req.digest() != csr.digest()) ||
        cert.isNull()
      ) && (
        !pending_csr.readPemFile(pending_csr_path) ||
        (req.digest() != pending_csr.digest())
      ))
  {
    std::cout << callsign << ": Add pending CSR '" << pending_csr_path
              << "' to CA" << std::endl;
    if (req.writePemFile(pending_csr_path))
    {
      const auto ca_op =
        pending_csr.isNull() ? "PENDING_CSR_CREATE" : "PENDING_CSR_UPDATE";
      runCAHook({
          { "CA_OP",      ca_op },
          { "CA_CSR_PEM", req.pem() }
        });
    }
    else
    {
      std::cerr << "*** WARNING: Could not write CSR file '"
                << pending_csr_path << "'" << std::endl;
    }
  }

  return cert;
} /* Reflector::csrReceived */


Json::Value& Reflector::clientStatus(const std::string& callsign)
{
  if (!m_status.isMember(callsign))
  {
    m_status["nodes"][callsign] = Json::Value(Json::objectValue);
  }
  return m_status["nodes"][callsign];
} /* Reflector::clientStatus */


/****************************************************************************
 *
 * Protected member functions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/

void Reflector::clientConnected(Async::FramedTcpConnection *con)
{
  std::cout << con->remoteHost() << ":" << con->remotePort()
       << ": Client connected" << endl;
  ReflectorClient *client = new ReflectorClient(this, con, m_cfg);
  con->verifyPeer.connect(sigc::mem_fun(*this, &Reflector::onVerifyPeer));
  m_client_con_map[con] = client;
} /* Reflector::clientConnected */


void Reflector::clientDisconnected(Async::FramedTcpConnection *con,
                           Async::FramedTcpConnection::DisconnectReason reason)
{
  ReflectorClientConMap::iterator it = m_client_con_map.find(con);
  assert(it != m_client_con_map.end());
  ReflectorClient *client = (*it).second;

  TGHandler::instance()->removeClient(client);

  if (!client->callsign().empty())
  {
    cout << client->callsign() << ": ";
  }
  else
  {
    std::cout << con->remoteHost() << ":" << con->remotePort() << ": ";
  }
  std::cout << "Client disconnected: "
            << TcpConnection::disconnectReasonStr(reason) << std::endl;

  m_client_con_map.erase(it);

  if (!client->callsign().empty())
  {
    m_status["nodes"].removeMember(client->callsign());
    broadcastMsg(MsgNodeLeft(client->callsign()),
        ReflectorClient::ExceptFilter(client));
  }
  //Application::app().runTask([=]{ delete client; });
  delete client;
} /* Reflector::clientDisconnected */


bool Reflector::udpCipherDataReceived(const IpAddress& addr, uint16_t port,
                                      void *buf, int count)
{
  if ((count <= 0) || (static_cast<size_t>(count) < UdpCipher::AADLEN))
  {
    std::cout << "### : Ignoring too short UDP datagram (" << count
              << " bytes)" << std::endl;
    return true;
  }

  stringstream ss;
  ss.write(reinterpret_cast<const char *>(buf), UdpCipher::AADLEN);
  if (!m_aad.unpack(ss))
  {
    std::cout << "### Ignoring UDP datagram with unparseable AAD" << std::endl;
    return true;
  }

  ReflectorClient* client = nullptr;
  if (m_aad.iv_cntr == 0)
  {
    UdpCipher::InitialAAD iaad;
    //std::cout << "### Reflector::udpCipherDataReceived: m_aad.iv_cntr="
    //          << m_aad.iv_cntr << std::endl;
    if (static_cast<size_t>(count) < iaad.packedSize())
    {
      std::cout << "### Reflector::udpCipherDataReceived: "
                   "Ignoring malformed UDP registration datagram" << std::endl;
      return true;
    }
    ss.clear();
    ss.write(reinterpret_cast<const char *>(buf)+UdpCipher::AADLEN,
        sizeof(UdpCipher::ClientId));

    Async::MsgPacker<UdpCipher::ClientId>::unpack(ss, iaad.client_id);
    //std::cout << "### Reflector::udpCipherDataReceived: client_id="
    //          << iaad.client_id << std::endl;
    auto client = ReflectorClient::lookup(iaad.client_id);
    if (client == nullptr)
    {
      std::cout << "### Could not find client id (" << iaad.client_id
                << ") specified in initial AAD datagram" << std::endl;
      return true;
    }
    m_udp_sock->setCipherIV(UdpCipher::IV{client->udpCipherIVRand(),
                                          client->clientId(), 0});
    m_udp_sock->setCipherKey(client->udpCipherKey());
    m_udp_sock->setCipherAADLength(iaad.packedSize());
  }
  else if ((client=ReflectorClient::lookup(std::make_pair(addr, port))))
  {
    //if (static_cast<size_t>(count) < UdpCipher::AADLEN)
    //{
    //  std::cout << "### Reflector::udpCipherDataReceived: Datagram too short "
    //               "to hold associated data" << std::endl;
    //  return true;
    //}

    //if (!aad_unpack_ok)
    //{
    //  std::cout << "*** WARNING: Unpacking associated data failed for UDP "
    //               "datagram from " << addr << ":" << port << std::endl;
    //  return true;
    //}
    //std::cout << "### Reflector::udpCipherDataReceived: m_aad.iv_cntr="
    //          << m_aad.iv_cntr << std::endl;
    m_udp_sock->setCipherIV(UdpCipher::IV{client->udpCipherIVRand(),
                                          client->clientId(), m_aad.iv_cntr});
    m_udp_sock->setCipherKey(client->udpCipherKey());
    m_udp_sock->setCipherAADLength(UdpCipher::AADLEN);
  }
  else
  {
    udpDatagramReceived(addr, port, nullptr, buf, count);
    return true;
  }

  return false;
} /* Reflector::udpCipherDataReceived */


void Reflector::udpDatagramReceived(const IpAddress& addr, uint16_t port,
                                    void* aadptr, void *buf, int count)
{
  //std::cout << "### Reflector::udpDatagramReceived:"
  //          << " addr=" << addr
  //          << " port=" << port
  //          << " count=" << count
  //          << std::endl;

  if (m_udp_sock->cipherAADLength() < UdpCipher::AADLEN)
  {
    std::cerr << "*** ERROR: Cipher AAD length too short" << std::endl;
    return;
  }

  stringstream ss;
  ss.write(reinterpret_cast<const char *>(buf), static_cast<size_t>(count));

  ReflectorUdpMsg header;
  if (!header.unpack(ss))
  {
    cout << "*** WARNING: Unpacking message header failed for UDP datagram "
            "from " << addr << ":" << port << endl;
    return;
  }
  ReflectorUdpMsgV2 header_v2;

  ReflectorClient* client = nullptr;
  UdpCipher::AAD aad;
  if (aadptr != nullptr)
  {
    //std::cout << "### Reflector::udpDatagramReceived: m_aad.iv_cntr="
    //          << m_aad.iv_cntr << std::endl;

    stringstream aadss;
    aadss.write(reinterpret_cast<const char *>(aadptr),
        m_udp_sock->cipherAADLength());

    if (!aad.unpack(aadss))
    {
      return;
    }
    if (aad.iv_cntr == 0) // Client UDP registration
    {
      UdpCipher::InitialAAD iaad;
      aadss.seekg(0);
      if (!aadss.good()) return;
      if (!iaad.unpack(aadss))
      {
        std::cout << "### Reflector::udpDatagramReceived: "
                     "Could not unpack iaad" << std::endl;
        return;
      }
      if (iaad.iv_cntr != 0) return;
      //std::cout << "### Reflector::udpDatagramReceived: iaad.client_id="
      //          << iaad.client_id << std::endl;
      client = ReflectorClient::lookup(iaad.client_id);
      if (client == nullptr)
      {
        std::cout << "### Reflector::udpDatagramReceived: Could not find "
                     "client id " << iaad.client_id << std::endl;
        return;
      }
      else if (client->remoteUdpPort() == 0)
      {
        //client->setRemoteUdpPort(port);
      }
      else
      {
        std::cout << "### Reflector::udpDatagramReceived: Client "
                  << iaad.client_id << " already registered." << std::endl;
      }
      client->setUdpRxSeq(0);
      //client->sendUdpMsg(MsgUdpHeartbeat());
    }
    else
    {
      client = ReflectorClient::lookup(std::make_pair(addr, port));
      if (client == nullptr)
      {
        std::cout << "### Unknown client " << addr << ":" << port << std::endl;
        return;
      }
    }
  }
  else
  {
    ss.seekg(0);
    if (!header_v2.unpack(ss))
    {
      std::cout << "*** WARNING: Unpacking V2 message header failed for UDP "
              "datagram from " << addr << ":" << port << std::endl;
      return;
    }
    client = ReflectorClient::lookup(header_v2.clientId());
    if (client == nullptr)
    {
      std::cerr << "*** WARNING: Incoming V2 UDP datagram from " << addr << ":"
           << port << " has invalid client id " << header_v2.clientId()
           << std::endl;
      return;
    }

    if (addr != client->remoteHost())
    {
      cerr << "*** WARNING[" << client->callsign()
           << "]: Incoming UDP packet has the wrong source ip, "
           << addr << " instead of " << client->remoteHost() << endl;
      return;
    }
  }

  //auto client = ReflectorClient::lookup(std::make_pair(addr, port));
  //if (client == nullptr)
  //{
  //  client = ReflectorClient::lookup(header.clientId());
  //  if (client == nullptr)
  //  {
  //    cerr << "*** WARNING: Incoming UDP datagram from " << addr << ":" << port
  //         << " has invalid client id " << header.clientId() << endl;
  //    return;
  //  }
  //}

  if (client->remoteUdpPort() == 0)
  {
    client->setRemoteUdpSource(std::make_pair(addr, port));
    client->sendUdpMsg(MsgUdpHeartbeat());
  }
  if (port != client->remoteUdpPort())
  {
    cerr << "*** WARNING[" << client->callsign()
         << "]: Incoming UDP packet has the wrong source UDP "
            "port number, " << port << " instead of "
         << client->remoteUdpPort() << endl;
    return;
  }

    // Check sequence number
  if (client->protoVer() >= ProtoVer(3, 0))
  {
    if (aad.iv_cntr < client->nextUdpRxSeq()) // Frame out of sequence (ignore)
    {
      std::cout << client->callsign()
                << ": Dropping out of sequence UDP frame with seq="
                << aad.iv_cntr << std::endl;
      return;
    }
    else if (aad.iv_cntr > client->nextUdpRxSeq()) // Frame lost
    {
      std::cout << client->callsign() << ": UDP frame(s) lost. Expected seq="
                << client->nextUdpRxSeq()
                << " but received " << aad.iv_cntr
                << ". Resetting next expected sequence number to "
                << (aad.iv_cntr + 1) << std::endl;
    }
    client->setUdpRxSeq(aad.iv_cntr + 1);
  }
  else
  {
    uint16_t next_udp_rx_seq = client->nextUdpRxSeq() & 0xffff;
    uint16_t udp_rx_seq_diff = header_v2.sequenceNum() - next_udp_rx_seq;
    if (udp_rx_seq_diff > 0x7fff) // Frame out of sequence (ignore)
    {
      std::cout << client->callsign()
                << ": Dropping out of sequence frame with seq="
                << header_v2.sequenceNum() << ". Expected seq="
                << next_udp_rx_seq << std::endl;
      return;
    }
    else if (udp_rx_seq_diff > 0) // Frame(s) lost
    {
      cout << client->callsign()
           << ": UDP frame(s) lost. Expected seq=" << next_udp_rx_seq
           << ". Received seq=" << header_v2.sequenceNum() << endl;
    }
    client->setUdpRxSeq(header_v2.sequenceNum() + 1);
  }

  client->udpMsgReceived(header);

  //std::cout << "### Reflector::udpDatagramReceived: type="
  //          << header.type() << std::endl;
  switch (header.type())
  {
    case MsgUdpHeartbeat::TYPE:
      break;

    case MsgUdpAudio::TYPE:
    {
      if (!client->isBlocked())
      {
        MsgUdpAudio msg;
        if (!msg.unpack(ss))
        {
          cerr << "*** WARNING[" << client->callsign()
               << "]: Could not unpack incoming MsgUdpAudioV1 message" << endl;
          return;
        }
        uint32_t tg = TGHandler::instance()->TGForClient(client);
        if (!msg.audioData().empty() && (tg > 0))
        {
          ReflectorClient* talker = TGHandler::instance()->talkerForTG(tg);
          if ((talker == 0) &&
              !TGHandler::instance()->hasTrunkTalker(tg))
          {
            TGHandler::instance()->setTalkerForTG(tg, client);
            talker = TGHandler::instance()->talkerForTG(tg);
          }
          if (talker == client)
          {
            TGHandler::instance()->setTalkerForTG(tg, client);
            broadcastUdpMsg(msg,
                ReflectorClient::mkAndFilter(
                  ReflectorClient::ExceptFilter(client),
                  ReflectorClient::TgFilter(tg)));
            if (m_is_satellite && m_satellite_client != nullptr)
            {
              m_satellite_client->onLocalAudio(tg, msg.audioData());
            }
            else
            {
              for (auto* link : m_trunk_links)
              {
                link->onLocalAudio(tg, msg.audioData());
              }
              for (auto& kv : m_satellite_con_map)
              {
                kv.second->onParentAudio(tg, msg.audioData());
              }
            }
          }
        }
      }
      break;
    }

    //case MsgUdpAudio::TYPE:
    //{
    //  if (!client->isBlocked())
    //  {
    //    MsgUdpAudio msg;
    //    if (!msg.unpack(ss))
    //    {
    //      cerr << "*** WARNING[" << client->callsign()
    //           << "]: Could not unpack incoming MsgUdpAudio message" << endl;
    //      return;
    //    }
    //    if (!msg.audioData().empty())
    //    {
    //      if (m_talker == 0)
    //      {
    //        setTalker(client);
    //        cout << m_talker->callsign() << ": Talker start on TG #"
    //             << msg.tg() << endl;
    //      }
    //      if (m_talker == client)
    //      {
    //        gettimeofday(&m_last_talker_timestamp, NULL);
    //        broadcastUdpMsgExcept(tg, client, msg,
    //            ProtoVerRange(ProtoVer(2, 0), ProtoVer::max()));
    //        MsgUdpAudioV1 msg_v1(msg.audioData());
    //        broadcastUdpMsgExcept(tg, client, msg_v1,
    //            ProtoVerRange(ProtoVer(0, 6),
    //                          ProtoVer(1, ProtoVer::max().minor())));
    //      }
    //    }
    //  }
    //  break;
    //}

    case MsgUdpFlushSamples::TYPE:
    {
      uint32_t tg = TGHandler::instance()->TGForClient(client);
      ReflectorClient* talker = TGHandler::instance()->talkerForTG(tg);
      if ((tg > 0) && (client == talker))
      {
        TGHandler::instance()->setTalkerForTG(tg, 0);
        if (m_is_satellite && m_satellite_client != nullptr)
        {
          m_satellite_client->onLocalFlush(tg);
        }
        else
        {
          for (auto* link : m_trunk_links)
          {
            link->onLocalFlush(tg);
          }
          for (auto& kv : m_satellite_con_map)
          {
            kv.second->onParentFlush(tg);
          }
        }
      }
        // To be 100% correct the reflector should wait for all connected
        // clients to send a MsgUdpAllSamplesFlushed message but that will
        // probably lead to problems, especially on reflectors with many
        // clients. We therefore acknowledge the flush immediately here to
        // the client who sent the flush request.
      client->sendUdpMsg(MsgUdpAllSamplesFlushed());
      break;
    }

    case MsgUdpAllSamplesFlushed::TYPE:
      // Ignore
      break;

    case MsgUdpSignalStrengthValues::TYPE:
    {
      if (!client->isBlocked())
      {
        MsgUdpSignalStrengthValues msg;
        if (!msg.unpack(ss))
        {
          cerr << "*** WARNING[" << client->callsign()
               << "]: Could not unpack incoming "
                  "MsgUdpSignalStrengthValues message" << endl;
          return;
        }
        typedef MsgUdpSignalStrengthValues::Rxs::const_iterator RxsIter;
        for (RxsIter it = msg.rxs().begin(); it != msg.rxs().end(); ++it)
        {
          const MsgUdpSignalStrengthValues::Rx& rx = *it;
          //std::cout << "### MsgUdpSignalStrengthValues:"
          //  << " id=" << rx.id()
          //  << " siglev=" << rx.siglev()
          //  << " enabled=" << rx.enabled()
          //  << " sql_open=" << rx.sqlOpen()
          //  << " active=" << rx.active()
          //  << std::endl;
          client->setRxSiglev(rx.id(), rx.siglev());
          client->setRxEnabled(rx.id(), rx.enabled());
          client->setRxSqlOpen(rx.id(), rx.sqlOpen());
          client->setRxActive(rx.id(), rx.active());
        }
      }
      break;
    }

    default:
      // Better ignoring unknown messages to make it easier to add messages to
      // the protocol but still be backwards compatible

      //cerr << "*** WARNING[" << client->callsign()
      //     << "]: Unknown UDP protocol message received: msg_type="
      //     << header.type() << endl;
      break;
  }
} /* Reflector::udpDatagramReceived */


void Reflector::onTalkerUpdated(uint32_t tg, ReflectorClient* old_talker,
                                ReflectorClient *new_talker)
{
  if (old_talker != 0)
  {
    cout << old_talker->callsign() << ": Talker stop on TG #" << tg << endl;
    old_talker->updateIsTalker();
    broadcastMsg(MsgTalkerStop(tg, old_talker->callsign()),
        ReflectorClient::mkAndFilter(
          ge_v2_client_filter,
          ReflectorClient::mkOrFilter(
            ReflectorClient::TgFilter(tg),
            ReflectorClient::TgMonitorFilter(tg))));
    if (tg == tgForV1Clients())
    {
      broadcastMsg(MsgTalkerStopV1(old_talker->callsign()), v1_client_filter);
    }
    broadcastUdpMsg(MsgUdpFlushSamples(),
          ReflectorClient::mkAndFilter(
            ReflectorClient::TgFilter(tg),
            ReflectorClient::ExceptFilter(old_talker)));
  }
  if (new_talker != 0)
  {
    cout << new_talker->callsign() << ": Talker start on TG #" << tg << endl;
    new_talker->updateIsTalker();
    broadcastMsg(MsgTalkerStart(tg, new_talker->callsign()),
        ReflectorClient::mkAndFilter(
          ge_v2_client_filter,
          ReflectorClient::mkOrFilter(
            ReflectorClient::TgFilter(tg),
            ReflectorClient::TgMonitorFilter(tg))));
    if (tg == tgForV1Clients())
    {
      broadcastMsg(MsgTalkerStartV1(new_talker->callsign()), v1_client_filter);
    }
  }

  // In satellite mode, forward to parent instead of trunk peers
  if (m_is_satellite && m_satellite_client != nullptr)
  {
    if (new_talker != nullptr)
    {
      m_satellite_client->onLocalTalkerStart(tg, new_talker->callsign());
    }
    else
    {
      m_satellite_client->onLocalTalkerStop(tg);
    }
  }
  else
  {
    // Notify trunk peers about local talker changes.
    for (auto* link : m_trunk_links)
    {
      if (new_talker != nullptr)
      {
        link->onLocalTalkerStart(tg, new_talker->callsign());
      }
      else
      {
        link->onLocalTalkerStop(tg);
      }
    }
    // Notify connected satellites
    for (auto& kv : m_satellite_con_map)
    {
      if (new_talker != nullptr)
      {
        kv.second->onParentTalkerStart(tg, new_talker->callsign());
      }
      else
      {
        kv.second->onParentTalkerStop(tg);
      }
    }
  }
} /* Reflector::onTalkerUpdated */


void Reflector::httpRequestReceived(Async::HttpServerConnection *con,
                                    Async::HttpServerConnection::Request& req)
{
  //std::cout << "### " << req.method << " " << req.target << std::endl;

  Async::HttpServerConnection::Response res;
  if ((req.method != "GET") && (req.method != "HEAD"))
  {
    res.setCode(501);
    Json::Value err_json;
    err_json["msg"] = req.method + ": Method not implemented";
    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = "";
    res.setContent("application/json", Json::writeString(wbuilder, err_json));
    con->write(res);
    return;
  }

  if (req.target != "/status")
  {
    res.setCode(404);
    res.setContent("application/json",
        "{\"msg\":\"Not found!\"}");
    con->write(res);
    return;
  }

  // Build trunk status fresh on each request (live state)
  Json::Value trunks(Json::objectValue);
  for (auto* link : m_trunk_links)
  {
    trunks[link->section()] = link->statusJson();
  }
  m_status["trunks"] = trunks;

  Json::Value cluster_arr(Json::arrayValue);
  for (uint32_t tg : m_cluster_tgs)
  {
    cluster_arr.append(tg);
  }
  m_status["cluster_tgs"] = cluster_arr;

  {
    Json::Value sats(Json::objectValue);
    for (auto& kv : m_satellite_con_map)
    {
      auto* sat = kv.second;
      // Skip satellites pending cleanup (heartbeat timed out)
      if (std::find(m_sat_cleanup_pending.begin(),
                    m_sat_cleanup_pending.end(),
                    sat) != m_sat_cleanup_pending.end())
      {
        continue;
      }
      if (!sat->satelliteId().empty())
      {
        sats[sat->satelliteId()] = sat->statusJson();
      }
    }
    if (sats.empty())
    {
      m_status.removeMember("satellites");
    }
    else
    {
      m_status["satellites"] = sats;
    }
  }

  // Static configuration
  m_status["version"] = SVXREFLECTOR_VERSION;

  {
    std::string local_prefix_str;
    m_cfg->getValue("GLOBAL", "LOCAL_PREFIX", local_prefix_str);
    Json::Value lp_arr(Json::arrayValue);
    std::istringstream ss(local_prefix_str);
    std::string token;
    while (std::getline(ss, token, ','))
    {
      token.erase(0, token.find_first_not_of(" \t"));
      token.erase(token.find_last_not_of(" \t") + 1);
      if (!token.empty()) lp_arr.append(token);
    }
    m_status["local_prefix"] = lp_arr;
  }

  std::string listen_port("5300");
  m_cfg->getValue("GLOBAL", "LISTEN_PORT", listen_port);
  m_status["listen_port"] = listen_port;

  std::string http_port;
  if (m_cfg->getValue("GLOBAL", "HTTP_SRV_PORT", http_port))
  {
    m_status["http_port"] = http_port;
  }

  if (m_is_satellite)
  {
    m_status["mode"] = "satellite";
    std::string sat_of, sat_port_str, sat_id;
    m_cfg->getValue("GLOBAL", "SATELLITE_OF", sat_of);
    m_cfg->getValue("GLOBAL", "SATELLITE_PORT", sat_port_str);
    m_cfg->getValue("GLOBAL", "SATELLITE_ID", sat_id);
    Json::Value sat_cfg(Json::objectValue);
    sat_cfg["parent_host"] = sat_of;
    if (!sat_port_str.empty()) sat_cfg["parent_port"] = sat_port_str;
    if (!sat_id.empty()) sat_cfg["id"] = sat_id;
    m_status["satellite"] = sat_cfg;
  }
  else
  {
    m_status["mode"] = "reflector";
    std::string sat_listen_port;
    if (m_cfg->getValue("SATELLITE", "LISTEN_PORT", sat_listen_port))
    {
      Json::Value sat_srv(Json::objectValue);
      sat_srv["listen_port"] = sat_listen_port;
      sat_srv["connected_count"] =
          static_cast<Json::UInt>(m_satellite_con_map.size());
      m_status["satellite_server"] = sat_srv;
    }
  }

  std::ostringstream os;
  Json::StreamWriterBuilder builder;
  builder["commentStyle"] = "None";
  builder["indentation"] = ""; //The JSON document is written on a single line
  Json::StreamWriter* writer = builder.newStreamWriter();
  writer->write(m_status, &os);
  delete writer;

  res.setContent("application/json", os.str());
  res.setSendContent(req.method == "GET");
  res.setCode(200);
  con->write(res);
} /* Reflector::requestReceived */


void Reflector::httpClientConnected(Async::HttpServerConnection *con)
{
  //std::cout << "### HTTP Client connected: "
  //          << con->remoteHost() << ":" << con->remotePort() << std::endl;
  con->requestReceived.connect(sigc::mem_fun(*this, &Reflector::httpRequestReceived));
} /* Reflector::httpClientConnected */


void Reflector::httpClientDisconnected(Async::HttpServerConnection *con,
    Async::HttpServerConnection::DisconnectReason reason)
{
  //std::cout << "### HTTP Client disconnected: "
  //          << con->remoteHost() << ":" << con->remotePort()
  //          << ": " << Async::HttpServerConnection::disconnectReasonStr(reason)
  //          << std::endl;
} /* Reflector::httpClientDisconnected */


void Reflector::onRequestAutoQsy(uint32_t from_tg)
{
  uint32_t tg = nextRandomQsyTg();
  if (tg == 0) { return; }

  std::cout << "Requesting auto-QSY from TG #" << from_tg
            << " to TG #" << tg << std::endl;

  broadcastMsg(MsgRequestQsy(tg),
      ReflectorClient::mkAndFilter(
        ge_v2_client_filter,
        ReflectorClient::TgFilter(from_tg)));
} /* Reflector::onRequestAutoQsy */


uint32_t Reflector::nextRandomQsyTg(void)
{
  if (m_random_qsy_tg == 0)
  {
    std::cout << "*** WARNING: QSY request for random TG "
              << "requested but RANDOM_QSY_RANGE is empty" << std::endl;
    return 0;
  }

  assert (m_random_qsy_tg != 0);
  uint32_t range_size = m_random_qsy_hi-m_random_qsy_lo+1;
  uint32_t i;
  for (i=0; i<range_size; ++i)
  {
    m_random_qsy_tg = (m_random_qsy_tg < m_random_qsy_hi) ?
      m_random_qsy_tg+1 : m_random_qsy_lo;
    if (TGHandler::instance()->clientsForTG(m_random_qsy_tg).empty())
    {
      return m_random_qsy_tg;
    }
  }

  std::cout << "*** WARNING: No random TG available for QSY" << std::endl;
  return 0;
} /* Reflector::nextRandomQsyTg */


void Reflector::ctrlPtyDataReceived(const void *buf, size_t count)
{
  const char* ptr = reinterpret_cast<const char*>(buf);
  const std::string cmdline(ptr, ptr + count);
  //std::cout << "### Reflector::ctrlPtyDataReceived: " << cmdline
  //          << std::endl;
  std::istringstream ss(cmdline);
  std::ostringstream errss;
  std::string cmd;
  if (!(ss >> cmd))
  {
    errss << "Invalid PTY command '" << cmdline << "'";
    goto write_status;
  }
  std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

  if (cmd == "CFG")
  {
    std::string section, tag, value;
    ss >> section >> tag >> value;
    if (!value.empty())
    {
      m_cfg->setValue(section, tag, value);
      m_cmd_pty->write("Trying to set config : " + section + "/" +
                       tag + "=" + value + "\n");
    }
    else if (!tag.empty())
    {
      m_cmd_pty->write("Get config : " + section + "/" +
                       tag + "=\"" + m_cfg->getValue(section, tag) + "\"\n");
    }
    else if (!section.empty())
    {
      m_cmd_pty->write("Section: \n\t" + section + "\n");
      for (const auto& tag : m_cfg->listSection(section))
      {
          m_cmd_pty->write("\t" + tag +
                           "=\"" + m_cfg->getValue(section, tag) + "\"\n");
      }
    }
    else
    {
      for (const auto& section : m_cfg->listSections())
      {
        m_cmd_pty->write("Section: \n\t" + section + "\n");
        for (const auto& tag : m_cfg->listSection(section))
        {
          m_cmd_pty->write("\t\t" + tag +
                           "=\"" + m_cfg->getValue(section, tag) + "\"\n");
        }
        m_cmd_pty->write("\n");
      }
    }
  }
  else if (cmd == "NODE")
  {
    std::string subcmd, callsign;
    unsigned blocktime;
    if (!(ss >> subcmd >> callsign >> blocktime))
    {
      errss << "Invalid NODE PTY command '" << cmdline << "'. "
               "Usage: NODE BLOCK <callsign> <blocktime seconds>";
      goto write_status;
    }
    std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::toupper);
    if (subcmd == "BLOCK")
    {
      auto node = ReflectorClient::lookup(callsign);
      if (node == nullptr)
      {
        errss << "Could not find node " << callsign;
        goto write_status;
      }
      node->setBlock(blocktime);
    }
    else
    {
      errss << "Invalid NODE PTY command '" << cmdline << "'. "
               "Usage: NODE BLOCK <callsign> <blocktime seconds>";
      goto write_status;
    }
  }
  else if (cmd == "CA")
  {
    std::string subcmd;
    if (!(ss >> subcmd))
    {
      errss << "Invalid CA PTY command '" << cmdline << "'. "
               "Usage: CA LS|LSC|LSP|SIGN <callsign>|RM <callsign>";
      goto write_status;
    }
    std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::toupper);
    if (subcmd == "SIGN")
    {
      std::string cn;
      if (!(ss >> cn))
      {
        errss << "Invalid CA SIGN PTY command '" << cmdline << "'. "
                 "Usage: CA SIGN <callsign>";
        goto write_status;
      }
      auto cert = signClientCsr(cn);
      if (!cert.isNull())
      {
        m_cmd_pty->write("---------- Signed Client Certificate ----------\n");
        m_cmd_pty->write(cert.toString());
        m_cmd_pty->write("-----------------------------------------------\n");
        std::cout << "---------- Signed Client Certificate ----------\n"
                  << cert.toString()
                  << "-----------------------------------------------"
                  << std::endl;
      }
      else
      {
        errss << "Certificate signing failed";
      }
    }
    else if (subcmd == "RM")
    {
      std::string cn;
      if (!(ss >> cn))
      {
        errss << "Invalid CA RM PTY command '" << cmdline << "'. "
                 "Usage: CA RM <callsign>";
        goto write_status;
      }
      if (removeClientCertFiles(cn))
      {
        std::string msg(cn + ": Removed client certificate and CSR");
        m_cmd_pty->write(msg + "\n");
        std::cout << msg << std::endl;
      }
      else
      {
        errss << "Failed to remove certificate and CSR for '" << cn << "'";
      }
    }
    else if (subcmd == "LS")
    {
        // List all certs and pending CSRs
      std::string certs = formatCerts();
      m_cmd_pty->write(certs);
    }
    else if (subcmd == "LSC")
    {
        // List only certificates
      std::string certs = formatCerts(true, false);
      m_cmd_pty->write(certs);
    }
    else if (subcmd == "LSP")
    {
        // List only pending CSRs
      std::string certs = formatCerts(false, true);
      m_cmd_pty->write(certs);
    }
    // FIXME: Implement when we have CRL support
    //else if (subcmd == "REVOKE")
    //{
    //}
    else
    {
      errss << "Invalid CA PTY command '" << cmdline << "'. "
               "Usage: CA LS|LSC|LSP|SIGN <callsign>|RM <callsign>";
      goto write_status;
    }
  }
  else
  {
    errss << "Valid commands are: CFG, NODE, CA\n"
          << "Usage:\n"
          << "CFG <section> <tag> <value>\n"
          << "NODE BLOCK <callsign> <blocktime seconds>\n"
          << "CA LS|LSC|LSP|SIGN <callsign>|RM <callsign>\n"
          << "\nEmpty CFG lists all configuration";
  }

  write_status:
    if (!errss.str().empty())
    {
      std::cerr << "*** ERROR: " << errss.str() << std::endl;
      m_cmd_pty->write(std::string("ERR:") + errss.str() + "\n");
      return;
    }
    m_cmd_pty->write("OK\n");
} /* Reflector::ctrlPtyDataReceived */


void Reflector::cfgUpdated(const std::string& section, const std::string& tag)
{
  std::string value;
  if (!m_cfg->getValue(section, tag, value))
  {
    std::cout << "*** ERROR: Failed to read updated configuration variable '"
              << section << "/" << tag << "'" << std::endl;
    return;
  }

  if (section == "GLOBAL")
  {
    if (tag == "SQL_TIMEOUT_BLOCKTIME")
    {
      unsigned t = TGHandler::instance()->sqlTimeoutBlocktime();
      if (!SvxLink::setValueFromString(t, value))
      {
        std::cout << "*** ERROR: Failed to set updated configuration "
                     "variable '" << section << "/" << tag << "'" << std::endl;
        return;
      }
      TGHandler::instance()->setSqlTimeoutBlocktime(t);
      //std::cout << "### New value for " << tag << "=" << t << std::endl;
    }
    else if (tag == "SQL_TIMEOUT")
    {
      unsigned t = TGHandler::instance()->sqlTimeout();
      if (!SvxLink::setValueFromString(t, value))
      {
        std::cout << "*** ERROR: Failed to set updated configuration "
                     "variable '" << section << "/" << tag << "'" << std::endl;
        return;
      }
      TGHandler::instance()->setSqlTimeout(t);
      //std::cout << "### New value for " << tag << "=" << t << std::endl;
    }
  }
} /* Reflector::cfgUpdated */


void Reflector::initTrunkLinks(void)
{
  // Collect local prefixes for cluster TG validation
  std::string local_prefix_str;
  m_cfg->getValue("GLOBAL", "LOCAL_PREFIX", local_prefix_str);
  std::vector<std::string> all_prefixes;
  {
    std::istringstream ss(local_prefix_str);
    std::string token;
    while (std::getline(ss, token, ','))
    {
      token.erase(0, token.find_first_not_of(" \t"));
      token.erase(token.find_last_not_of(" \t") + 1);
      if (!token.empty()) all_prefixes.push_back(token);
    }
  }

  // First pass: collect all remote prefixes so we can do longest-prefix-match
  std::vector<std::string> trunk_sections;
  for (const auto& section : m_cfg->listSections())
  {
    if (section.substr(0, 6) != "TRUNK_")
    {
      continue;
    }
    trunk_sections.push_back(section);

    std::string remote_prefix_str;
    m_cfg->getValue(section, "REMOTE_PREFIX", remote_prefix_str);
    {
      std::istringstream ss(remote_prefix_str);
      std::string token;
      while (std::getline(ss, token, ','))
      {
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (!token.empty()) all_prefixes.push_back(token);
      }
    }
  }

  // Second pass: create trunk links with full prefix knowledge
  for (const auto& section : trunk_sections)
  {
    auto* link = new TrunkLink(this, *m_cfg, section);
    if (link->initialize())
    {
      link->setAllPrefixes(all_prefixes);
      m_trunk_links.push_back(link);
    }
    else
    {
      std::cerr << "*** ERROR: Failed to initialize trunk link '"
                << section << "'" << std::endl;
      delete link;
    }
  }

  // Validate cluster TGs don't overlap with any prefix
  for (uint32_t tg : m_cluster_tgs)
  {
    std::string s = std::to_string(tg);
    for (const auto& prefix : all_prefixes)
    {
      if (s.size() >= prefix.size() &&
          s.compare(0, prefix.size(), prefix) == 0)
      {
        std::cerr << "*** WARNING: Cluster TG " << tg
                  << " conflicts with prefix " << prefix
                  << " — this TG will be routed as cluster (broadcast to all)"
                  << std::endl;
      }
    }
  }
} /* Reflector::initTrunkLinks */


void Reflector::initTrunkServer(void)
{
  if (m_trunk_links.empty())
  {
    return;  // No trunk links configured — no need for a trunk server
  }

  std::string trunk_port("5302");
  m_cfg->getValue("GLOBAL", "TRUNK_LISTEN_PORT", trunk_port);

  m_trunk_srv = new TcpServer<FramedTcpConnection>(trunk_port);
  m_trunk_srv->clientConnected.connect(
      sigc::mem_fun(*this, &Reflector::trunkClientConnected));
  m_trunk_srv->clientDisconnected.connect(
      sigc::mem_fun(*this, &Reflector::trunkClientDisconnected));

  std::cout << "Trunk server listening on port " << trunk_port << std::endl;
} /* Reflector::initTrunkServer */


void Reflector::trunkClientConnected(Async::FramedTcpConnection* con)
{
  // Per-IP pending limit: allow at most 2 pending (pre-hello) connections per
  // source IP to prevent reconnect storms where rapid retries from one peer
  // fill the global pending pool and starve other peers.  A limit of 2 (rather
  // than 1) tolerates one overlapping connection during normal reconnects.
  const Async::IpAddress remote_ip = con->remoteHost();
  unsigned ip_pending = 0;
  for (const auto& kv : m_trunk_pending_cons)
  {
    if (kv.first->remoteHost() == remote_ip)
    {
      ++ip_pending;
    }
  }
  if (ip_pending >= 2)
  {
    std::cerr << "*** WARNING: TRUNK inbound from " << remote_ip
              << ": too many pending connections from this IP — rejecting"
              << std::endl;
    con->disconnect();
    return;
  }

  // Reject connections beyond the global pending limit to prevent fd exhaustion
  if (m_trunk_pending_cons.size() >= TRUNK_MAX_PENDING_CONS)
  {
    std::cerr << "*** WARNING: TRUNK inbound from " << remote_ip
              << ": too many pending connections — rejecting" << std::endl;
    con->disconnect();
    return;
  }

  std::cout << "TRUNK: Inbound connection from "
            << con->remoteHost() << ":" << con->remotePort() << std::endl;

  if (m_trunk_debug)
  {
    std::cout << "TRUNK [DEBUG]: pending_count=" << m_trunk_pending_cons.size()
              << " ip_pending=" << ip_pending
              << " from " << remote_ip << std::endl;
  }

  // Set up a timeout for receiving the hello message
  auto* timer = new Async::Timer(10000, Async::Timer::TYPE_ONESHOT);
  timer->expired.connect(
      sigc::mem_fun(*this, &Reflector::trunkPendingTimeout));
  m_trunk_pending_cons[con] = timer;

  // Use a limited frame size — only a hello message is expected.
  // MsgTrunkHello contains strings + HMAC, so needs more than PREAUTH (64).
  // Cap at SSL_SETUP size (4096) which is plenty for a hello.
  con->setMaxFrameSize(ReflectorMsg::MAX_SSL_SETUP_FRAME_SIZE);
  con->frameReceived.connect(
      sigc::mem_fun(*this, &Reflector::trunkPendingFrameReceived));
} /* Reflector::trunkClientConnected */


void Reflector::trunkClientDisconnected(Async::FramedTcpConnection* con,
    Async::FramedTcpConnection::DisconnectReason reason)
{
  // Check pending (pre-hello) connections
  auto pit = m_trunk_pending_cons.find(con);
  if (pit != m_trunk_pending_cons.end())
  {
    delete pit->second;  // timer
    m_trunk_pending_cons.erase(pit);
    std::cout << "TRUNK: Pending inbound from "
              << con->remoteHost() << " disconnected" << std::endl;
    return;
  }

  // Check handed-off connections
  auto tit = m_trunk_inbound_map.find(con);
  if (tit != m_trunk_inbound_map.end())
  {
    tit->second->onInboundDisconnected(con, reason);
    m_trunk_inbound_map.erase(tit);
  }
} /* Reflector::trunkClientDisconnected */


void Reflector::trunkPendingFrameReceived(Async::FramedTcpConnection* con,
                                           std::vector<uint8_t>& data)
{
  auto pit = m_trunk_pending_cons.find(con);
  if (pit == m_trunk_pending_cons.end())
  {
    return;  // Not a pending connection (already handed off)
  }

  auto buf = reinterpret_cast<const char*>(data.data());
  std::stringstream ss;
  ss.write(buf, data.size());

  // Helper: clean up the pending entry and disconnect.
  // TcpConnection::disconnect() does NOT emit the disconnected signal,
  // so trunkClientDisconnected would never fire — we must clean up the
  // pending map entry ourselves before calling disconnect().
  auto rejectPending = [&]()
  {
    delete pit->second;  // timer
    m_trunk_pending_cons.erase(pit);
    con->disconnect();
  };

  ReflectorMsg header;
  if (!header.unpack(ss))
  {
    std::cerr << "*** ERROR: TRUNK inbound: failed to unpack message header"
              << std::endl;
    rejectPending();
    return;
  }

  if (header.type() != MsgTrunkHello::TYPE)
  {
    std::cerr << "*** WARNING: TRUNK inbound: expected MsgTrunkHello, got type="
              << header.type() << std::endl;
    rejectPending();
    return;
  }

  MsgTrunkHello msg;
  if (!msg.unpack(ss))
  {
    std::cerr << "*** ERROR: TRUNK inbound: failed to unpack MsgTrunkHello"
              << std::endl;
    rejectPending();
    return;
  }

  if (msg.id().empty())
  {
    std::cerr << "*** ERROR: TRUNK inbound: peer sent empty trunk ID"
              << std::endl;
    rejectPending();
    return;
  }

  // Sanitize peer ID for safe logging (strip control chars)
  std::string safe_id;
  for (char c : msg.id())
  {
    if (c >= 0x20 && c < 0x7f)
    {
      safe_id += c;
    }
  }
  if (safe_id.size() > 64)
  {
    safe_id.resize(64);
  }

  // Find the matching TrunkLink by section name, shared secret, and prefix.
  // Both sides must use the same [TRUNK_x] section name — sysops agree on a
  // shared link name. The peer's hello ID is its section name.
  TrunkLink* matched_link = nullptr;
  for (auto* link : m_trunk_links)
  {
    // Section name must match (case-sensitive)
    if (msg.id() != link->section())
    {
      continue;
    }

    if (!msg.verify(link->secret()))
    {
      if (m_trunk_debug)
      {
        std::cout << "TRUNK [DEBUG]: peer '" << safe_id
                  << "' section matches " << link->section()
                  << " but HMAC mismatch" << std::endl;
      }
      std::cerr << "*** ERROR: TRUNK inbound: peer '" << safe_id
                << "' section matches " << link->section()
                << " but authentication failed (wrong secret)"
                << std::endl;
      rejectPending();
      return;
    }

    // Check if the peer's local_prefix matches this link's remote_prefix.
    // The peer sends a comma-separated prefix string; we compare the sorted
    // sets for equality.
    const auto& expected = link->remotePrefix();
    std::vector<std::string> peer_prefixes;
    {
      std::istringstream pss(msg.localPrefix());
      std::string tok;
      while (std::getline(pss, tok, ','))
      {
        tok.erase(0, tok.find_first_not_of(" \t"));
        tok.erase(tok.find_last_not_of(" \t") + 1);
        if (!tok.empty()) peer_prefixes.push_back(tok);
      }
    }
    std::vector<std::string> sorted_expected(expected);
    std::vector<std::string> sorted_peer(peer_prefixes);
    std::sort(sorted_expected.begin(), sorted_expected.end());
    std::sort(sorted_peer.begin(), sorted_peer.end());
    if (sorted_expected == sorted_peer)
    {
      matched_link = link;
      break;
    }
    else
    {
      std::string exp_str, peer_str;
      for (const auto& p : sorted_expected)
      {
        if (!exp_str.empty()) exp_str += ",";
        exp_str += p;
      }
      for (const auto& p : sorted_peer)
      {
        if (!peer_str.empty()) peer_str += ",";
        peer_str += p;
      }
      std::cerr << "*** ERROR: TRUNK inbound: peer '" << safe_id
                << "' section matches " << link->section()
                << " but prefix mismatch: expected=[" << exp_str
                << "] got=[" << peer_str << "]" << std::endl;
      rejectPending();
      return;
    }
  }

  if (matched_link == nullptr)
  {
    std::cerr << "*** ERROR: TRUNK inbound: peer '" << safe_id
              << "' no matching section name"
              << std::endl;
    rejectPending();
    return;
  }

  if (m_trunk_debug)
  {
    std::cout << "TRUNK [DEBUG]: peer '" << safe_id
              << "' matched to " << matched_link->section() << std::endl;
  }

  // Clean up the pending entry
  delete pit->second;  // timer
  m_trunk_pending_cons.erase(pit);

  // Disconnect the pending frame handler (Reflector will no longer handle
  // frames for this connection — the TrunkLink takes over)
  con->frameReceived.clear();

  // Upgrade to full frame size now that the peer is authenticated
  con->setMaxFrameSize(ReflectorMsg::MAX_POSTAUTH_FRAME_SIZE);

  // Hand off to the TrunkLink
  m_trunk_inbound_map[con] = matched_link;
  matched_link->acceptInboundConnection(con, msg);
} /* Reflector::trunkPendingFrameReceived */


void Reflector::trunkPendingTimeout(Async::Timer* t)
{
  // Find which pending connection this timer belongs to
  for (auto it = m_trunk_pending_cons.begin();
       it != m_trunk_pending_cons.end(); ++it)
  {
    if (it->second == t)
    {
      std::cerr << "*** WARNING: TRUNK inbound from "
                << it->first->remoteHost()
                << ": hello timeout — disconnecting" << std::endl;
      auto* con = it->first;
      delete it->second;  // timer
      m_trunk_pending_cons.erase(it);
      con->disconnect();
      return;
    }
  }
} /* Reflector::trunkPendingTimeout */


void Reflector::initSatelliteServer(void)
{
  std::string sat_port;
  std::string sat_secret;
  if (!m_cfg->getValue("SATELLITE", "LISTEN_PORT", sat_port) ||
      !m_cfg->getValue("SATELLITE", "SECRET", sat_secret) ||
      sat_secret.empty())
  {
    return;  // No [SATELLITE] section — not accepting satellites
  }

  m_satellite_secret = sat_secret;
  m_sat_srv = new TcpServer<FramedTcpConnection>(sat_port);
  m_sat_srv->clientConnected.connect(
      sigc::mem_fun(*this, &Reflector::satelliteConnected));
  m_sat_srv->clientDisconnected.connect(
      sigc::mem_fun(*this, &Reflector::satelliteDisconnected));

  std::cout << "Satellite server listening on port " << sat_port << std::endl;
} /* Reflector::initSatelliteServer */


void Reflector::satelliteConnected(Async::FramedTcpConnection* con)
{
  std::cout << "SAT: Inbound connection from "
            << con->remoteHost() << ":" << con->remotePort() << std::endl;
  auto* link = new SatelliteLink(this, con, m_satellite_secret);
  link->linkFailed.connect(
      sigc::mem_fun(*this, &Reflector::onSatelliteLinkFailed));
  m_satellite_con_map[con] = link;
} /* Reflector::satelliteConnected */


void Reflector::satelliteDisconnected(Async::FramedTcpConnection* con,
    Async::FramedTcpConnection::DisconnectReason reason)
{
  auto it = m_satellite_con_map.find(con);
  if (it != m_satellite_con_map.end())
  {
    std::cout << "SAT: Satellite '" << it->second->satelliteId()
              << "' disconnected" << std::endl;
    delete it->second;
    m_satellite_con_map.erase(it);
  }
} /* Reflector::satelliteDisconnected */


void Reflector::onSatelliteLinkFailed(SatelliteLink* link)
{
  m_sat_cleanup_pending.push_back(link);
  m_sat_cleanup_timer.setEnable(false);
  m_sat_cleanup_timer.setTimeout(0);
  m_sat_cleanup_timer.setEnable(true);
} /* Reflector::onSatelliteLinkFailed */


void Reflector::processSatelliteCleanup(Async::Timer* t)
{
  std::vector<SatelliteLink*> pending;
  pending.swap(m_sat_cleanup_pending);

  for (SatelliteLink* link : pending)
  {
    Async::FramedTcpConnection* con = nullptr;
    for (auto& kv : m_satellite_con_map)
    {
      if (kv.second == link)
      {
        con = kv.first;
        break;
      }
    }
    if (con == nullptr) continue;

    std::cout << "SAT: Cleaning up timed-out satellite '"
              << link->satelliteId() << "'" << std::endl;
    delete link;
    m_satellite_con_map.erase(con);
    con->disconnect();
  }
} /* Reflector::processSatelliteCleanup */


void Reflector::forwardSatelliteAudioToTrunks(uint32_t tg,
                                               const std::string& callsign)
{
  for (auto* link : m_trunk_links)
  {
    link->onLocalTalkerStart(tg, callsign);
  }
} /* Reflector::forwardSatelliteAudioToTrunks */


void Reflector::forwardSatelliteStopToTrunks(uint32_t tg)
{
  for (auto* link : m_trunk_links)
  {
    link->onLocalTalkerStop(tg);
  }
} /* Reflector::forwardSatelliteStopToTrunks */


void Reflector::forwardSatelliteRawAudioToTrunks(uint32_t tg,
    const std::vector<uint8_t>& audio)
{
  for (auto* link : m_trunk_links)
  {
    link->onLocalAudio(tg, audio);
  }
} /* Reflector::forwardSatelliteRawAudioToTrunks */


void Reflector::forwardSatelliteFlushToTrunks(uint32_t tg)
{
  for (auto* link : m_trunk_links)
  {
    link->onLocalFlush(tg);
  }
} /* Reflector::forwardSatelliteFlushToTrunks */


void Reflector::forwardAudioToSatellitesExcept(SatelliteLink* except,
    uint32_t tg, const std::vector<uint8_t>& audio)
{
  for (auto& kv : m_satellite_con_map)
  {
    if (kv.second != except)
    {
      kv.second->onParentAudio(tg, audio);
    }
  }
} /* Reflector::forwardAudioToSatellitesExcept */


void Reflector::forwardFlushToSatellitesExcept(SatelliteLink* except,
    uint32_t tg)
{
  for (auto& kv : m_satellite_con_map)
  {
    if (kv.second != except)
    {
      kv.second->onParentFlush(tg);
    }
  }
} /* Reflector::forwardFlushToSatellitesExcept */


void Reflector::onTrunkTalkerUpdated(uint32_t tg,
                                     std::string old_cs, std::string new_cs)
{
  auto ge_v2_client_filter =
      ReflectorClient::ProtoVerLargerOrEqualFilter(ProtoVer(2, 0));

  if (!old_cs.empty())
  {
    std::cout << old_cs << ": Trunk talker stop on TG #" << tg << std::endl;
    broadcastMsg(MsgTalkerStop(tg, old_cs),
        ReflectorClient::mkAndFilter(
          ge_v2_client_filter,
          ReflectorClient::mkOrFilter(
            ReflectorClient::TgFilter(tg),
            ReflectorClient::TgMonitorFilter(tg))));
    broadcastUdpMsg(MsgUdpFlushSamples(),
        ReflectorClient::TgFilter(tg));
  }
  if (!new_cs.empty())
  {
    std::cout << new_cs << ": Trunk talker start on TG #" << tg << std::endl;
    broadcastMsg(MsgTalkerStart(tg, new_cs),
        ReflectorClient::mkAndFilter(
          ge_v2_client_filter,
          ReflectorClient::mkOrFilter(
            ReflectorClient::TgFilter(tg),
            ReflectorClient::TgMonitorFilter(tg))));
  }

  // Forward trunk talker events to connected satellites
  for (auto& kv : m_satellite_con_map)
  {
    if (!new_cs.empty())
    {
      kv.second->onParentTalkerStart(tg, new_cs);
    }
    else if (!old_cs.empty())
    {
      kv.second->onParentTalkerStop(tg);
    }
  }
} /* Reflector::onTrunkTalkerUpdated */


bool Reflector::loadCertificateFiles(void)
{
  if (!buildPath("GLOBAL", "CERT_PKI_DIR", SVX_LOCAL_STATE_DIR, m_pki_dir) ||
      !buildPath("GLOBAL", "CERT_CA_KEYS_DIR", m_pki_dir, m_keys_dir) ||
      !buildPath("GLOBAL", "CERT_CA_PENDING_CSRS_DIR", m_pki_dir,
                 m_pending_csrs_dir) ||
      !buildPath("GLOBAL", "CERT_CA_CSRS_DIR", m_pki_dir, m_csrs_dir) ||
      !buildPath("GLOBAL", "CERT_CA_CERTS_DIR", m_pki_dir, m_certs_dir))
  {
    return false;
  }

  if (!loadRootCAFiles() || !loadSigningCAFiles() ||
      !loadServerCertificateFiles())
  {
    return false;
  }

  if (!m_cfg->getValue("GLOBAL", "CERT_CA_BUNDLE", m_ca_bundle_file))
  {
    m_ca_bundle_file = m_pki_dir + "/ca-bundle.crt";
  }
  if (access(m_ca_bundle_file.c_str(), F_OK) != 0)
  {
    if (!ensureDirectoryExist(m_ca_bundle_file) ||
        !m_ca_cert.writePemFile(m_ca_bundle_file))
    {
      std::cout << "*** ERROR: Failed to write CA bundle file '"
                << m_ca_bundle_file << "'" << std::endl;
      return false;
    }
  }
  if (!m_ssl_ctx.setCaCertificateFile(m_ca_bundle_file))
  {
    std::cout << "*** ERROR: Failed to read CA certificate bundle '"
              << m_ca_bundle_file << "'" << std::endl;
    return false;
  }

  struct stat st;
  if (stat(m_ca_bundle_file.c_str(), &st) != 0)
  {
    auto errstr = SvxLink::strError(errno);
    std::cerr << "*** ERROR: Failed to read CA file from '"
              << m_ca_bundle_file << "': " << errstr << std::endl;
    return false;
  }
  auto bundle = caBundlePem();
  m_ca_size = bundle.size();
  Async::Digest ca_dgst;
  if (!ca_dgst.md(m_ca_md, MsgCABundle::MD_ALG, bundle))
  {
    std::cerr << "*** ERROR: CA bundle checksumming failed"
              << std::endl;
    return false;
  }
  ca_dgst.signInit(MsgCABundle::MD_ALG, m_issue_ca_pkey);
  m_ca_sig = ca_dgst.sign(bundle);
  //m_ca_url = "";
  //m_cfg->getValue("GLOBAL", "CERT_CA_URL", m_ca_url);

  return true;
} /* Reflector::loadCertificateFiles */


bool Reflector::loadServerCertificateFiles(void)
{
  std::string cert_cn;
  if (!m_cfg->getValue("SERVER_CERT", "COMMON_NAME", cert_cn) ||
      cert_cn.empty())
  {
    std::cerr << "*** ERROR: The 'SERVER_CERT/COMMON_NAME' variable is "
                 "unset which is needed for certificate signing request "
                 "generation." << std::endl;
    return false;
  }

  std::string keyfile;
  if (!m_cfg->getValue("SERVER_CERT", "KEYFILE", keyfile))
  {
    keyfile = m_keys_dir + "/" + cert_cn + ".key";
  }
  Async::SslKeypair pkey;
  if (access(keyfile.c_str(), F_OK) != 0)
  {
    std::cout << "Server private key file not found. Generating '"
              << keyfile << "'" << std::endl;
    if (!generateKeyFile(pkey, keyfile))
    {
      return false;
    }
  }
  else if (!pkey.readPrivateKeyFile(keyfile))
  {
    std::cerr << "*** ERROR: Failed to read private key file from '"
              << keyfile << "'" << std::endl;
    return false;
  }

  if (!m_cfg->getValue("SERVER_CERT", "CRTFILE", m_crtfile))
  {
    m_crtfile = m_certs_dir + "/" + cert_cn + ".crt";
  }
  Async::SslX509 cert;
  bool generate_cert = (access(m_crtfile.c_str(), F_OK) != 0);
  if (!generate_cert)
  {
    generate_cert = !cert.readPemFile(m_crtfile) ||
                    !cert.verify(m_issue_ca_pkey);
    if (generate_cert)
    {
      std::cerr << "*** WARNING: Failed to read server certificate "
                   "from '" << m_crtfile << "' or the cert is invalid. "
                   "Generating new certificate." << std::endl;
      cert.clear();
    }
    else
    {
      int days=0, seconds=0;
      cert.validityTime(days, seconds);
      //std::cout << "### days=" << days << "  seconds=" << seconds
      //          << std::endl;
      time_t tnow = time(NULL);
      time_t renew_time = tnow + (days*24*3600 + seconds)*RENEW_AFTER;
      if (!cert.timeIsWithinRange(tnow, renew_time))
      {
        std::cerr << "Time to renew the server certificate '" << m_crtfile
                  << "'. It's valid until "
                  << cert.notAfterLocaltimeString() << "." << std::endl;
        cert.clear();
        generate_cert = true;
      }
    }
  }
  if (generate_cert)
  {
    //if (!pkey_fresh && !generateKeyFile(pkey, keyfile))
    //{
    //  return false;
    //}

    std::string csrfile;
    if (!m_cfg->getValue("SERVER_CERT", "CSRFILE", csrfile))
    {
      csrfile = m_csrs_dir + "/" + cert_cn + ".csr";
    }
    Async::SslCertSigningReq req;
    std::cout << "Generating server certificate signing request file '"
              << csrfile << "'" << std::endl;
    req.setVersion(Async::SslCertSigningReq::VERSION_1);
    req.addSubjectName("CN", cert_cn);
    Async::SslX509Extensions req_exts;
    req_exts.addBasicConstraints("critical, CA:FALSE");
    req_exts.addKeyUsage(
        "critical, digitalSignature, keyEncipherment, keyAgreement");
    req_exts.addExtKeyUsage("serverAuth");
    std::stringstream csr_san_ss;
    csr_san_ss << "DNS:" << cert_cn;
    std::string cert_san_str;
    if (m_cfg->getValue("SERVER_CERT", "SUBJECT_ALT_NAME", cert_san_str) &&
        !cert_san_str.empty())
    {
      csr_san_ss << "," << cert_san_str;
    }
    std::string email_address;
    if (m_cfg->getValue("SERVER_CERT", "EMAIL_ADDRESS", email_address) &&
        !email_address.empty())
    {
      csr_san_ss << ",email:" << email_address;
    }
    req_exts.addSubjectAltNames(csr_san_ss.str());
    req.addExtensions(req_exts);
    req.setPublicKey(pkey);
    req.sign(pkey);
    if (!req.writePemFile(csrfile))
    {
      // FIXME: Read SSL error stack

      std::cerr << "*** WARNING: Failed to write server certificate "
                   "signing request file to '" << csrfile << "'"
                << std::endl;
      //return false;
    }
    std::cout << "-------- Certificate Signing Request -------" << std::endl;
    req.print();
    std::cout << "--------------------------------------------" << std::endl;

    std::cout << "Generating server certificate file '" << m_crtfile << "'"
              << std::endl;
    cert.setSerialNumber();
    cert.setVersion(Async::SslX509::VERSION_3);
    cert.setIssuerName(m_issue_ca_cert.subjectName());
    cert.setSubjectName(req.subjectName());
    cert.setValidityTime(CERT_VALIDITY_DAYS);
    cert.addExtensions(req.extensions());
    cert.setPublicKey(pkey);
    cert.sign(m_issue_ca_pkey);
    assert(cert.verify(m_issue_ca_pkey));
    if (!ensureDirectoryExist(m_crtfile) || !cert.writePemFile(m_crtfile) ||
        !m_issue_ca_cert.appendPemFile(m_crtfile))
    {
      std::cout << "*** ERROR: Failed to write server certificate file '"
                << m_crtfile << "'" << std::endl;
      return false;
    }
  }
  std::cout << "------------ Server Certificate ------------" << std::endl;
  cert.print();
  std::cout << "--------------------------------------------" << std::endl;

  if (!m_ssl_ctx.setCertificateFiles(keyfile, m_crtfile))
  {
      std::cout << "*** ERROR: Failed to read and verify key ('"
                << keyfile << "') and certificate ('"
                << m_crtfile << "') files. "
                << "If key- and cert-file does not match, the certificate "
                   "is invalid for any other reason, you need "
                   "to remove the cert file in order to trigger the "
                   "generation of a new certificate signing request."
                   "Then the CSR need to be signed by the CA which creates a "
                   "valid certificate."
                << std::endl;
      return false;
  }

  startCertRenewTimer(cert, m_renew_cert_timer);

  return true;
} /* Reflector::loadServerCertificateFiles */


bool Reflector::generateKeyFile(Async::SslKeypair& pkey,
                                const std::string& keyfile)
{
  pkey.generate(2048);
  if (!ensureDirectoryExist(keyfile) || !pkey.writePrivateKeyFile(keyfile))
  {
    std::cerr << "*** ERROR: Failed to write private key file to '"
              << keyfile << "'" << std::endl;
    return false;
  }
  return true;
} /* Reflector::generateKeyFile */


bool Reflector::loadRootCAFiles(void)
{
    // Read root CA private key or generate a new one if it does not exist
  std::string ca_keyfile;
  if (!m_cfg->getValue("ROOT_CA", "KEYFILE", ca_keyfile))
  {
    ca_keyfile = m_keys_dir + "/svxreflector_root_ca.key";
  }
  if (access(ca_keyfile.c_str(), F_OK) != 0)
  {
    std::cout << "Root CA private key file not found. Generating '"
              << ca_keyfile << "'" << std::endl;
    if (!m_ca_pkey.generate(4096))
    {
      std::cout << "*** ERROR: Failed to generate root CA key" << std::endl;
      return false;
    }
    if (!ensureDirectoryExist(ca_keyfile) ||
        !m_ca_pkey.writePrivateKeyFile(ca_keyfile))
    {
      std::cerr << "*** ERROR: Failed to write root CA private key file to '"
                << ca_keyfile << "'" << std::endl;
      return false;
    }
  }
  else if (!m_ca_pkey.readPrivateKeyFile(ca_keyfile))
  {
    std::cerr << "*** ERROR: Failed to read root CA private key file from '"
              << ca_keyfile << "'" << std::endl;
    return false;
  }

    // Read the root CA certificate or generate a new one if it does not exist
  std::string ca_crtfile;
  if (!m_cfg->getValue("ROOT_CA", "CRTFILE", ca_crtfile))
  {
    ca_crtfile = m_certs_dir + "/svxreflector_root_ca.crt";
  }
  bool generate_ca_cert = (access(ca_crtfile.c_str(), F_OK) != 0);
  if (!generate_ca_cert)
  {
    if (!m_ca_cert.readPemFile(ca_crtfile) ||
        !m_ca_cert.verify(m_ca_pkey) ||
        !m_ca_cert.timeIsWithinRange())
    {
      std::cerr << "*** ERROR: Failed to read root CA certificate file "
                   "from '" << ca_crtfile << "' or the cert is invalid."
                << std::endl;
      return false;
    }
  }
  if (generate_ca_cert)
  {
    std::cout << "Generating root CA certificate file '" << ca_crtfile << "'"
              << std::endl;
    m_ca_cert.setSerialNumber();
    m_ca_cert.setVersion(Async::SslX509::VERSION_3);

    std::string value;
    value = "SvxReflector Root CA";
    (void)m_cfg->getValue("ROOT_CA", "COMMON_NAME", value);
    if (value.empty())
    {
      std::cerr << "*** ERROR: The 'ROOT_CA/COMMON_NAME' variable is "
                   "unset which is needed for root CA certificate generation."
                << std::endl;
      return false;
    }
    m_ca_cert.addIssuerName("CN", value);
    if (m_cfg->getValue("ROOT_CA", "ORG_UNIT", value) &&
        !value.empty())
    {
      m_ca_cert.addIssuerName("OU", value);
    }
    if (m_cfg->getValue("ROOT_CA", "ORG", value) && !value.empty())
    {
      m_ca_cert.addIssuerName("O", value);
    }
    if (m_cfg->getValue("ROOT_CA", "LOCALITY", value) &&
        !value.empty())
    {
      m_ca_cert.addIssuerName("L", value);
    }
    if (m_cfg->getValue("ROOT_CA", "STATE", value) && !value.empty())
    {
      m_ca_cert.addIssuerName("ST", value);
    }
    if (m_cfg->getValue("ROOT_CA", "COUNTRY", value) && !value.empty())
    {
      m_ca_cert.addIssuerName("C", value);
    }
    m_ca_cert.setSubjectName(m_ca_cert.issuerName());
    Async::SslX509Extensions ca_exts;
    ca_exts.addBasicConstraints("critical, CA:TRUE");
    ca_exts.addKeyUsage("critical, cRLSign, digitalSignature, keyCertSign");
    if (m_cfg->getValue("ROOT_CA", "EMAIL_ADDRESS", value) &&
        !value.empty())
    {
      ca_exts.addSubjectAltNames("email:" + value);
    }
    m_ca_cert.addExtensions(ca_exts);
    m_ca_cert.setValidityTime(ROOT_CA_VALIDITY_DAYS);
    m_ca_cert.setPublicKey(m_ca_pkey);
    m_ca_cert.sign(m_ca_pkey);
    if (!m_ca_cert.writePemFile(ca_crtfile))
    {
      std::cout << "*** ERROR: Failed to write root CA certificate file '"
                << ca_crtfile << "'" << std::endl;
      return false;
    }
  }
  std::cout << "----------- Root CA Certificate ------------" << std::endl;
  m_ca_cert.print();
  std::cout << "--------------------------------------------" << std::endl;

  return true;
} /* Reflector::loadRootCAFiles */


bool Reflector::loadSigningCAFiles(void)
{
    // Read issuing CA private key or generate a new one if it does not exist
  std::string ca_keyfile;
  if (!m_cfg->getValue("ISSUING_CA", "KEYFILE", ca_keyfile))
  {
    ca_keyfile = m_keys_dir + "/svxreflector_issuing_ca.key";
  }
  if (access(ca_keyfile.c_str(), F_OK) != 0)
  {
    std::cout << "Issuing CA private key file not found. Generating '"
              << ca_keyfile << "'" << std::endl;
    if (!m_issue_ca_pkey.generate(2048))
    {
      std::cout << "*** ERROR: Failed to generate CA key" << std::endl;
      return false;
    }
    if (!ensureDirectoryExist(ca_keyfile) ||
        !m_issue_ca_pkey.writePrivateKeyFile(ca_keyfile))
    {
      std::cerr << "*** ERROR: Failed to write issuing CA private key file "
                   "to '" << ca_keyfile << "'" << std::endl;
      return false;
    }
  }
  else if (!m_issue_ca_pkey.readPrivateKeyFile(ca_keyfile))
  {
    std::cerr << "*** ERROR: Failed to read issuing CA private key file "
                 "from '" << ca_keyfile << "'" << std::endl;
    return false;
  }

    // Read the CA certificate or generate a new one if it does not exist
  std::string ca_crtfile;
  if (!m_cfg->getValue("ISSUING_CA", "CRTFILE", ca_crtfile))
  {
    ca_crtfile = m_certs_dir + "/svxreflector_issuing_ca.crt";
  }
  bool generate_ca_cert = (access(ca_crtfile.c_str(), F_OK) != 0);
  if (!generate_ca_cert)
  {
    generate_ca_cert = !m_issue_ca_cert.readPemFile(ca_crtfile) ||
                       !m_issue_ca_cert.verify(m_ca_pkey) ||
                       !m_issue_ca_cert.timeIsWithinRange();
    if (generate_ca_cert)
    {
      std::cerr << "*** WARNING: Failed to read issuing CA certificate "
                   "from '" << ca_crtfile << "' or the cert is invalid. "
                   "Generating new certificate." << std::endl;
      m_issue_ca_cert.clear();
    }
    else
    {
      int days=0, seconds=0;
      m_issue_ca_cert.validityTime(days, seconds);
      time_t tnow = time(NULL);
      time_t renew_time = tnow + (days*24*3600 + seconds)*RENEW_AFTER;
      if (!m_issue_ca_cert.timeIsWithinRange(tnow, renew_time))
      {
        std::cerr << "Time to renew the issuing CA certificate '"
                  << ca_crtfile << "'. It's valid until "
                  << m_issue_ca_cert.notAfterLocaltimeString() << "."
                  << std::endl;
        m_issue_ca_cert.clear();
        generate_ca_cert = true;
      }
    }
  }

  if (generate_ca_cert)
  {
    std::string ca_csrfile;
    if (!m_cfg->getValue("ISSUING_CA", "CSRFILE", ca_csrfile))
    {
      ca_csrfile = m_csrs_dir + "/svxreflector_issuing_ca.csr";
    }
    std::cout << "Generating issuing CA CSR file '" << ca_csrfile
              << "'" << std::endl;
    Async::SslCertSigningReq csr;
    csr.setVersion(Async::SslCertSigningReq::VERSION_1);
    std::string value;
    value = "SvxReflector Issuing CA";
    (void)m_cfg->getValue("ISSUING_CA", "COMMON_NAME", value);
    if (value.empty())
    {
      std::cerr << "*** ERROR: The 'ISSUING_CA/COMMON_NAME' variable is "
                   "unset which is needed for issuing CA certificate "
                   "generation." << std::endl;
      return false;
    }
    csr.addSubjectName("CN", value);
    if (m_cfg->getValue("ISSUING_CA", "ORG_UNIT", value) &&
        !value.empty())
    {
      csr.addSubjectName("OU", value);
    }
    if (m_cfg->getValue("ISSUING_CA", "ORG", value) && !value.empty())
    {
      csr.addSubjectName("O", value);
    }
    if (m_cfg->getValue("ISSUING_CA", "LOCALITY", value) && !value.empty())
    {
      csr.addSubjectName("L", value);
    }
    if (m_cfg->getValue("ISSUING_CA", "STATE", value) && !value.empty())
    {
      csr.addSubjectName("ST", value);
    }
    if (m_cfg->getValue("ISSUING_CA", "COUNTRY", value) && !value.empty())
    {
      csr.addSubjectName("C", value);
    }
    Async::SslX509Extensions exts;
    exts.addBasicConstraints("critical, CA:TRUE, pathlen:0");
    exts.addKeyUsage("critical, cRLSign, digitalSignature, keyCertSign");
    if (m_cfg->getValue("ISSUING_CA", "EMAIL_ADDRESS", value) &&
        !value.empty())
    {
      exts.addSubjectAltNames("email:" + value);
    }
    csr.addExtensions(exts);
    csr.setPublicKey(m_issue_ca_pkey);
    csr.sign(m_issue_ca_pkey);
    //csr.print();
    if (!csr.writePemFile(ca_csrfile))
    {
      std::cout << "*** ERROR: Failed to write issuing CA CSR file '"
                << ca_csrfile << "'" << std::endl;
      return false;
    }

    std::cout << "Generating issuing CA certificate file '" << ca_crtfile
              << "'" << std::endl;
    m_issue_ca_cert.setSerialNumber();
    m_issue_ca_cert.setVersion(Async::SslX509::VERSION_3);
    m_issue_ca_cert.setSubjectName(csr.subjectName());
    m_issue_ca_cert.addExtensions(csr.extensions());
    m_issue_ca_cert.setValidityTime(ISSUING_CA_VALIDITY_DAYS);
    m_issue_ca_cert.setPublicKey(m_issue_ca_pkey);
    m_issue_ca_cert.setIssuerName(m_ca_cert.subjectName());
    m_issue_ca_cert.sign(m_ca_pkey);
    if (!m_issue_ca_cert.writePemFile(ca_crtfile))
    {
      std::cout << "*** ERROR: Failed to write issuing CA certificate file '"
                << ca_crtfile << "'" << std::endl;
      return false;
    }
  }
  std::cout << "---------- Issuing CA Certificate ----------" << std::endl;
  m_issue_ca_cert.print();
  std::cout << "--------------------------------------------" << std::endl;

  startCertRenewTimer(m_issue_ca_cert, m_renew_issue_ca_cert_timer);

  return true;
} /* Reflector::loadSigningCAFiles */


bool Reflector::onVerifyPeer(TcpConnection *con, bool preverify_ok,
                             X509_STORE_CTX *x509_store_ctx)
{
  //std::cout << "### Reflector::onVerifyPeer: preverify_ok="
  //          << (preverify_ok ? "yes" : "no") << std::endl;

  Async::SslX509 cert(*x509_store_ctx);
  preverify_ok = preverify_ok && !cert.isNull();
  preverify_ok = preverify_ok && !cert.commonName().empty();
  if (!preverify_ok)
  {
    std::cout << "*** ERROR: Certificate verification failed for client"
              << std::endl;
    std::cout << "------------ Client Certificate -------------" << std::endl;
    cert.print();
    std::cout << "---------------------------------------------" << std::endl;
  }

  return preverify_ok;
} /* Reflector::onVerifyPeer */


bool Reflector::buildPath(const std::string& sec,    const std::string& tag,
                          const std::string& defdir, std::string& defpath)
{
  bool isdir = (defpath.back() == '/');
  std::string path(defpath);
  if (!m_cfg->getValue(sec, tag, path) || path.empty())
  {
    path = defpath;
  }
  //std::cout << "### sec=" << sec << "  tag=" << tag << "  defdir=" << defdir << "  defpath=" << defpath << "  path=" << path << std::endl;
  if ((path.front() != '/') && (path.front() != '.'))
  {
    path = defdir + "/" + defpath;
  }
  if (!ensureDirectoryExist(path))
  {
    return false;
  }
  if (isdir && (path.back() == '/'))
  {
    defpath = path.substr(0, path.size()-1);
  }
  else
  {
    defpath = std::move(path);
  }
  //std::cout << "### defpath=" << defpath << std::endl;
  return true;
} /* Reflector::buildPath */


bool Reflector::removeClientCertFiles(const std::string& cn)
{
  if (!isCallsignPathSafe(cn))
  {
    std::cerr << "*** ERROR: Unsafe callsign in removeClientCertFiles" << std::endl;
    return false;
  }

  std::vector<std::string> paths = {
    m_csrs_dir + "/" + cn + ".csr",
    m_pending_csrs_dir + "/" + cn + ".csr",
    m_certs_dir + "/" + cn + ".crt"
  };

  bool success = true;
  size_t path_unlink_cnt = 0;
  for (const auto& path : paths)
  {
    if (unlink(path.c_str()) == 0)
    {
      path_unlink_cnt += 1;
    }
    else if (errno != ENOENT)
    {
      success = false;
      auto errstr = SvxLink::strError(errno);
      std::cerr << "*** WARNING: Failed to remove file '" << path << "': "
                << errstr << std::endl;
    }
  }

  return success && (path_unlink_cnt > 0);
} /* Reflector::removeClientCertFiles */


void Reflector::runCAHook(const Async::Exec::Environment& env)
{
  auto ca_hook_cmd = m_cfg->getValue("GLOBAL", "CERT_CA_HOOK");
  if (!ca_hook_cmd.empty())
  {
    auto ca_hook = new Async::Exec(ca_hook_cmd);
    ca_hook->addEnvironmentVars(env);
    ca_hook->setTimeout(300); // Five minutes timeout
    ca_hook->stdoutData.connect(
        [=](const char* buf, int cnt)
        {
          std::cout << buf;
        });
    ca_hook->stderrData.connect(
        [=](const char* buf, int cnt)
        {
          std::cerr << buf;
        });
    ca_hook->exited.connect(
        [=](void) {
          if (ca_hook->ifExited())
          {
            if (ca_hook->exitStatus() != 0)
            {
              std::cerr << "*** ERROR: CA hook exited with exit status "
                        << ca_hook->exitStatus() << std::endl;
            }
          }
          else if (ca_hook->ifSignaled())
          {
            std::cerr << "*** ERROR: CA hook exited with signal "
                      << ca_hook->termSig() << std::endl;
          }
          Async::Application::app().runTask([=]{ delete ca_hook; });
        });
    ca_hook->run();
  }
} /* Reflector::runCAHook */


std::vector<CertInfo> Reflector::getAllCerts(void)
{
  std::vector<CertInfo> certs;

  DIR* dir = opendir(m_certs_dir.c_str());
  if (dir != nullptr)
  {
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
      std::string filename(entry->d_name);
      if (filename.length() > 4 &&
          filename.substr(filename.length() - 4) == ".crt")
      {
        std::string callsign = filename.substr(0, filename.length() - 4);
        Async::SslX509 cert = loadClientCertificate(callsign);
        if (!cert.isNull() && callsignOk(callsign, false))
        {
          CertInfo info;
          info.callsign = cert.commonName();
          info.is_signed = true;
          info.valid_until = cert.notAfterLocaltimeString();
          info.not_after = cert.notAfter();
          info.received_time = 0;

          certs.push_back(info);
        }
      }
    }
    closedir(dir);

    std::sort(certs.begin(), certs.end(),
        [](const CertInfo& a, const CertInfo& b)
        {
          return a.callsign < b.callsign;
        });
  }

  return certs;
} /* Reflector::getAllCerts */


std::vector<CertInfo> Reflector::getAllPendingCSRs(void)
{
  std::vector<CertInfo> certs;

  DIR* dir = opendir(m_pending_csrs_dir.c_str());
  if (dir != nullptr)
  {
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
      std::string filename(entry->d_name);
      if (filename.length() > 4 &&
          filename.substr(filename.length() - 4) == ".csr")
      {
        std::string callsign = filename.substr(0, filename.length() - 4);
        Async::SslCertSigningReq csr = loadClientPendingCsr(callsign);
        if (!csr.isNull())
        {
          CertInfo info;
          info.callsign = csr.commonName();
          info.is_signed = false;
          info.valid_until = "";
          info.not_after = 0;

            // Extract email addresses, might be useful to contact user or
            // check against a database
          const auto san = csr.extensions().subjectAltName();
          if (!san.isNull())
          {
            san.forEach(
                [&](int type, std::string value)
                {
                  info.emails.push_back(value);
                },
                GEN_EMAIL);
          }

            // Get file timestamp
          std::string csr_path = m_pending_csrs_dir + "/" + callsign + ".csr";
          struct stat st;
          if (stat(csr_path.c_str(), &st) == 0)
          {
            info.received_time = st.st_mtime;
          }
          else
          {
            info.received_time = 0;
          }

          certs.push_back(info);
        }
      }
    }
    closedir(dir);

    std::sort(certs.begin(), certs.end(),
        [](const CertInfo& a, const CertInfo& b)
        {
          return a.callsign < b.callsign;
        });
  }

  return certs;
} /* Reflector::getAllPendingCSRs */


std::string Reflector::formatCerts(bool signedCerts, bool pendingCerts)
{
  std::ostringstream ss;

  if (signedCerts && pendingCerts)
  {
    ss << "------------ All Certificates/CSRs ------------\n";
  }
  else if (signedCerts && !pendingCerts)
  {
    ss << "------------- Signed Certificates -------------\n";
  }
  else if (!signedCerts && pendingCerts)
  {
    ss << "---------------- Pending CSRs -----------------\n";
  }
  else
  {
    return "ERR:Neither certificates nor CSRs requested\n";
  }

  auto signed_certs_list = getAllCerts();
  auto pending_certs_list = getAllPendingCSRs();
  if (signedCerts)
  {
    ss << "Signed Certificates:\n";

    if (signed_certs_list.empty())
    {
      ss << "\t(none)\n";
    }
    else
    {
      size_t max_cn_len = 0;
      for (const auto& info : signed_certs_list)
      {
        if (info.callsign.size() > max_cn_len)
        {
          max_cn_len = info.callsign.size();
        }
      }
      for (const auto& info : signed_certs_list)
      {
        ss << "\t" << std::left << std::setw(max_cn_len) << info.callsign
           << "  Valid until: " << info.valid_until << "\n";
      }
    }
  }

  if (pendingCerts)
  {
    ss << "Pending CSRs (awaiting signing):\n";

    if (pending_certs_list.empty())
    {
      ss << "\t(none)\n";
    }
    else
    {
      size_t max_cn_len = 0;
      for (const auto& info : pending_certs_list)
      {
        if (info.callsign.size() > max_cn_len)
        {
          max_cn_len = info.callsign.size();
        }
      }
      for (const auto& info : pending_certs_list)
      {
        ss << "\t" << std::left << std::setw(max_cn_len) << info.callsign;
        if (!info.emails.empty())
        {
            // Join all emails in one string
          std::string emails_str = std::accumulate(
            std::next(info.emails.begin()),
            info.emails.end(),
            info.emails.empty() ? std::string() : info.emails[0],
            [](const std::string& a, const std::string& b)
            {
              return a + ", " + b;
            });
          ss << "  Email: " << emails_str;
        }
        ss << "\n";
      }
    }
  }

  ss << "-----------------------------------------------\n";
  return ss.str();
} /* Reflector::formatCerts */


/*
 * This file has not been truncated
 */
