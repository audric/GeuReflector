/**
 * @file   TgFilter.h
 * @brief  Flexible TG filter: exact, prefix (*), range (-)
 * @author DJ1JAY / FM-Funknetz
 *
 * Syntax (comma-separated):
 *   24*        — prefix match: all TGs starting with "24"
 *   2427-2438  — range: TG 2427..2438 inclusive
 *   26200      — exact match: only TG 26200
 *
 * Empty filter = match all.
 *
 * Example:
 *   TgFilter f = TgFilter::parse("24*,2427-2438,26200");
 *   f.matches(24)    → true
 *   f.matches(2430)  → true
 *   f.matches(26200) → true
 *   f.matches(26201) → false
 */

#ifndef TGFILTER_H
#define TGFILTER_H

#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>

struct TgFilter
{
  struct Entry
  {
    enum Type { EXACT, PREFIX, RANGE } type;
    uint32_t    value     = 0;
    uint32_t    range_end = 0;
    std::string prefix;
  };

  std::vector<Entry> entries;
  bool empty(void) const { return entries.empty(); }

  static TgFilter parse(const std::string& s)
  {
    TgFilter f;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
      tok.erase(0, tok.find_first_not_of(" \t"));
      tok.erase(tok.find_last_not_of(" \t") + 1);
      if (tok.empty()) continue;

      Entry e;
      if (!tok.empty() && tok.back() == '*')
      {
        e.type   = Entry::PREFIX;
        e.prefix = tok.substr(0, tok.size() - 1);
      }
      else
      {
        auto dash = tok.find('-');
        if (dash != std::string::npos)
        {
          try {
            e.type      = Entry::RANGE;
            e.value     = std::stoul(tok.substr(0, dash));
            e.range_end = std::stoul(tok.substr(dash + 1));
          } catch (...) { continue; }
        }
        else
        {
          try {
            e.type  = Entry::EXACT;
            e.value = std::stoul(tok);
          } catch (...) { continue; }
        }
      }
      f.entries.push_back(e);
    }
    return f;
  }

  bool matches(uint32_t tg) const
  {
    if (entries.empty()) return true;
    const std::string s = std::to_string(tg);
    for (const auto& e : entries)
    {
      switch (e.type)
      {
        case Entry::EXACT:
          if (tg == e.value) return true;
          break;
        case Entry::PREFIX:
          if (s.size() >= e.prefix.size() &&
              s.compare(0, e.prefix.size(), e.prefix) == 0)
            return true;
          break;
        case Entry::RANGE:
          if (tg >= e.value && tg <= e.range_end) return true;
          break;
      }
    }
    return false;
  }

  std::string toString(void) const
  {
    std::string r;
    for (const auto& e : entries)
    {
      if (!r.empty()) r += ',';
      switch (e.type)
      {
        case Entry::EXACT:  r += std::to_string(e.value); break;
        case Entry::PREFIX: r += e.prefix + "*"; break;
        case Entry::RANGE:  r += std::to_string(e.value) + "-"
                               + std::to_string(e.range_end); break;
      }
    }
    return r;
  }
};

#endif /* TGFILTER_H */
