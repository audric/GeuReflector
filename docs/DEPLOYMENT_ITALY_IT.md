# Esempio di deployment nazionale — Italia

Questo documento descrive un deployment nazionale completo per l'Italia utilizzando
un'istanza di GeuReflector per regione, interconnesse in una topologia a maglia
piena (full-mesh).

## Perché il trunk è necessario

Un'istanza standard di SvxReflector è un server centralizzato unico. Senza il
trunk, un deployment nazionale dispone di sole due opzioni:

**Opzione A — un unico reflector nazionale:**
tutte e 20 le regioni collegano i propri nodi a un singolo server. Funziona, ma
**un solo talk group può essere attivo alla volta sull'intera rete nazionale**.
Mentre il Lazio è in QSO sul TG nazionale, nessun'altra regione può avere una
conversazione indipendente simultanea. Ogni trasmissione di ogni regione transita
per lo stesso server, e un'interruzione di quel server abbatte l'intera rete.

**Opzione B — reflector regionali indipendenti collegati da istanze SvxLink:**
è possibile collegare due reflector facendo girare un'istanza di SvxLink che si
connette ad entrambi come nodo client, fungendo da ponte audio. Tuttavia, ogni
istanza-ponte può gestire un solo TG alla volta, e serve un'istanza per ogni
coppia di reflector. Per 20 regioni in full mesh occorrono già 190 istanze SvxLink
solo per coprire un singolo TG condiviso — moltiplicate ulteriormente per ogni TG
aggiuntivo che si vuole condividere. La complessità operativa, il consumo di
risorse e la superficie di guasto derivanti dalla gestione di centinaia di processi
ponte rendono questo approccio impraticabile su scala nazionale.

**Con il trunk GeuReflector:**
ogni regione gestisce il proprio reflector indipendente (resilienza, autonomia
locale) e i link trunk li interconnettono permettendo di condividere i TG in modo
selettivo. Tutti e 20 i TG regionali possono ospitare QSO indipendenti simultanei,
mentre un TG nazionale dedicato è accessibile a tutti. Il trunk gestisce
automaticamente l'arbitraggio quando due regioni tentano di usare un TG condiviso
nello stesso momento.

| | Reflector unico | Reflector indipendenti | Trunk mesh GeuReflector |
|---|---|---|---|
| QSO regionali simultanei | No — un TG alla volta | Sì, ma un ponte per TG per coppia | Sì |
| Comunicazione inter-regionale | Sì | Sì, ma 190+ processi ponte | Sì |
| Autonomia regionale | No | Sì | Sì |
| Singolo punto di guasto | Sì | No | No |

---

## Numerazione dei TG

La numerazione dei TG utilizzata in questo documento è **ispirata al piano di
numerazione DMR italiano**, già ben strutturato, ampiamente adottato e riconosciuto
dalla comunità dei radioamatori. Riutilizzare questa struttura evita di dover
inventare un piano di numerazione alternativo e rende l'allocazione immediatamente
familiare agli operatori italiani.

**Questo sistema SvxLink/GeuReflector è completamente indipendente dal DMR e non
è connesso ad alcuna rete DMR.** I numeri di TG sono utilizzati qui esclusivamente
come spazio di identificatori comodo e significativo — non sono coinvolti radio,
hotspot o ripetitori DMR di alcun tipo.

I TG DMR italiani iniziano tutti con il prefisso nazionale `222`. Questo prefisso
viene rimosso quando si imposta `LOCAL_PREFIX`, quindi ogni reflector viene
configurato con il solo codice regionale a 2 cifre (es. TG `22201` → `LOCAL_PREFIX=01`).

Sia `LOCAL_PREFIX` che `REMOTE_PREFIX` accettano un elenco separato da virgole,
quindi una singola istanza può gestire più regioni
(es. `LOCAL_PREFIX=11,12,13` per Liguria, Piemonte e Valle d'Aosta).

---

## Inventario dei reflector

| Regione                   | TG      | LOCAL_PREFIX | Hostname suggerito                  |
|---------------------------|---------|--------------|-------------------------------------|
| LAZIO                     | 22201   | 01           | svxref-lazio.example.it             |
| SARDEGNA                  | 22202   | 02           | svxref-sardegna.example.it          |
| UMBRIA                    | 22203   | 03           | svxref-umbria.example.it            |
| LIGURIA                   | 22211   | 11           | svxref-liguria.example.it           |
| PIEMONTE                  | 22212   | 12           | svxref-piemonte.example.it          |
| VALLE D'AOSTA             | 22213   | 13           | svxref-valledaosta.example.it       |
| LOMBARDIA                 | 22221   | 21           | svxref-lombardia.example.it         |
| FRIULI VENEZIA GIULIA     | 22231   | 31           | svxref-friuli.example.it            |
| TRENTINO ALTO ADIGE       | 22232   | 32           | svxref-trentino.example.it          |
| VENETO                    | 22233   | 33           | svxref-veneto.example.it            |
| EMILIA ROMAGNA            | 22241   | 41           | svxref-emilia.example.it            |
| TOSCANA                   | 22251   | 51           | svxref-toscana.example.it           |
| ABRUZZO                   | 22261   | 61           | svxref-abruzzo.example.it           |
| MARCHE                    | 22262   | 62           | svxref-marche.example.it            |
| PUGLIA                    | 22271   | 71           | svxref-puglia.example.it            |
| BASILICATA                | 22281   | 81           | svxref-basilicata.example.it        |
| CALABRIA                  | 22282   | 82           | svxref-calabria.example.it          |
| CAMPANIA                  | 22283   | 83           | svxref-campania.example.it          |
| MOLISE                    | 22284   | 84           | svxref-molise.example.it            |
| SICILIA                   | 22291   | 91           | svxref-sicilia.example.it           |

---

## Topologia

20 reflector × 19 trunk link ciascuno = **190 connessioni TCP trunk** in totale (full mesh).
Ogni coppia condivide un `SECRET` univoco. La porta `5302` è utilizzata per tutte
le connessioni trunk; la porta `5300` rimane riservata ai nodi client SvxLink.

---

## Convenzione per i secret condivisi

Ogni coppia di reflector condivide un secret. Si consiglia una convenzione di
denominazione come `IT_<PREFISSO_A>_<PREFISSO_B>` (prefisso minore per primo):

```
IT_01_02   # LAZIO ↔ SARDEGNA
IT_01_03   # LAZIO ↔ UMBRIA
…
IT_84_91   # MOLISE ↔ SICILIA
```

---

## Configurazione: LAZIO (esempio completo)

```ini
[GLOBAL]
LISTEN_PORT=5300
LOCAL_PREFIX=01
CLUSTER_TGS=222
HTTP_SRV_PORT=8080
COMMAND_PTY=/dev/shm/reflector_ctrl

[TRUNK_01_02]
HOST=svxref-sardegna.example.it
PORT=5302
SECRET=IT_01_02
REMOTE_PREFIX=02

[TRUNK_01_03]
HOST=svxref-umbria.example.it
PORT=5302
SECRET=IT_01_03
REMOTE_PREFIX=03

[TRUNK_01_11]
HOST=svxref-liguria.example.it
PORT=5302
SECRET=IT_01_11
REMOTE_PREFIX=11

[TRUNK_01_12]
HOST=svxref-piemonte.example.it
PORT=5302
SECRET=IT_01_12
REMOTE_PREFIX=12

[TRUNK_01_13]
HOST=svxref-valledaosta.example.it
PORT=5302
SECRET=IT_01_13
REMOTE_PREFIX=13

[TRUNK_01_21]
HOST=svxref-lombardia.example.it
PORT=5302
SECRET=IT_01_21
REMOTE_PREFIX=21

[TRUNK_01_31]
HOST=svxref-friuli.example.it
PORT=5302
SECRET=IT_01_31
REMOTE_PREFIX=31

[TRUNK_01_32]
HOST=svxref-trentino.example.it
PORT=5302
SECRET=IT_01_32
REMOTE_PREFIX=32

[TRUNK_01_33]
HOST=svxref-veneto.example.it
PORT=5302
SECRET=IT_01_33
REMOTE_PREFIX=33

[TRUNK_01_41]
HOST=svxref-emilia.example.it
PORT=5302
SECRET=IT_01_41
REMOTE_PREFIX=41

[TRUNK_01_51]
HOST=svxref-toscana.example.it
PORT=5302
SECRET=IT_01_51
REMOTE_PREFIX=51

[TRUNK_01_61]
HOST=svxref-abruzzo.example.it
PORT=5302
SECRET=IT_01_61
REMOTE_PREFIX=61

[TRUNK_01_62]
HOST=svxref-marche.example.it
PORT=5302
SECRET=IT_01_62
REMOTE_PREFIX=62

[TRUNK_01_71]
HOST=svxref-puglia.example.it
PORT=5302
SECRET=IT_01_71
REMOTE_PREFIX=71

[TRUNK_01_81]
HOST=svxref-basilicata.example.it
PORT=5302
SECRET=IT_01_81
REMOTE_PREFIX=81

[TRUNK_01_82]
HOST=svxref-calabria.example.it
PORT=5302
SECRET=IT_01_82
REMOTE_PREFIX=82

[TRUNK_01_83]
HOST=svxref-campania.example.it
PORT=5302
SECRET=IT_01_83
REMOTE_PREFIX=83

[TRUNK_01_84]
HOST=svxref-molise.example.it
PORT=5302
SECRET=IT_01_84
REMOTE_PREFIX=84

[TRUNK_01_91]
HOST=svxref-sicilia.example.it
PORT=5302
SECRET=IT_01_91
REMOTE_PREFIX=91
```

Tutte le altre configurazioni regionali seguono lo stesso schema: impostare
`LOCAL_PREFIX` al codice a 2 cifre della propria regione e aggiungere una sezione
`[TRUNK_xx_yy]` per ciascuna delle altre 19 regioni, dove `xx` e `yy` sono la
coppia ordinata di codici regionali (il minore per primo). I nomi delle sezioni
devono essere identici su entrambi i lati del link — ad esempio il collegamento
tra Sardegna (02) e Umbria (03) si chiama `[TRUNK_02_03]` su entrambi i reflector.
Usare il `SECRET` corrispondente secondo la convenzione sopra indicata.

---

## TG Cluster

L'impostazione `CLUSTER_TGS` abilita i talk group nazionali che vengono
trasmessi a **tutti** i peer trunk indipendentemente dalla proprietà del
prefisso. In questo deployment, il TG 222 (il canale di chiamata nazionale
italiano nella numerazione DMR) è configurato come TG cluster su ogni
reflector:

```ini
CLUSTER_TGS=222
```

Quando un qualsiasi client su un qualsiasi reflector regionale trasmette sul
TG 222, l'audio viene inviato a tutti gli altri 19 reflector simultaneamente.
A differenza dei TG basati su prefisso (che instradano verso un singolo
reflector proprietario), i TG cluster non hanno proprietario — qualsiasi
reflector può originare una trasmissione. L'arbitraggio del talker funziona
allo stesso modo (tie-break tramite nonce) se due operatori trasmettono
contemporaneamente.

Tutti i reflector nella mesh devono avere lo stesso valore di `CLUSTER_TGS`.

---

## Conversazioni simultanee

Non esiste **nessun limite fisso** al numero di QSO simultanei sul trunk. Il trunk
tra ogni coppia di reflector è una singola connessione TCP che multiplexa tutti i
TG attivi contemporaneamente — ogni frame `MsgTrunkAudio` è etichettato con il
numero di TG, quindi qualsiasi numero di TG può trasmettere audio nello stesso momento.

L'unica regola per TG è che **un solo operatore alla volta può trasmettere su un
dato TG** (gestito dalla logica di arbitraggio). Più TG possono avere trasmissioni
attive simultaneamente senza interferire.

### Stima della banda necessaria

| Codec | Bitrate per TG attivo | TG simultanei su un trunk da 1 Mbps |
|-------|-----------------------|--------------------------------------|
| OPUS  | ~8–16 kbps            | ~60–125                              |
| GSM   | ~13 kbps              | ~75                                  |

In questo deployment italiano, anche se tutti e 20 i TG regionali fossero
simultaneamente in QSO, il carico totale sul trunk per ogni reflector rimarrebbe
ampiamente sotto 1 Mbps. La banda non è un fattore limitante a questa scala;
il vero limite pratico è il numero di operatori patentati attivi in un dato momento.

---

## Reflector satellite (opzionale)

Le regioni più grandi possono eseguire istanze aggiuntive come satellite
(ad esempio una per provincia) per distribuire il carico client senza aggiungere
ulteriori connessioni trunk. Un satellite si collega al proprio reflector
regionale e inoltra tutto il traffico — i reflector remoti vedono i client
satellite come se fossero connessi direttamente al reflector regionale.

**Reflector regionale** (es. Lazio) — aggiungere una sezione `[SATELLITE]`:
```ini
[SATELLITE]
LISTEN_PORT=5303
SECRET=lazio_satellite_secret
```

**Satellite** (es. provincia di Roma):
```ini
[GLOBAL]
SATELLITE_OF=svxref-lazio.example.it
SATELLITE_PORT=5303
SATELLITE_SECRET=lazio_satellite_secret
SATELLITE_ID=sat-roma
```

Il satellite non imposta `LOCAL_PREFIX`, `REMOTE_PREFIX` o alcuna sezione
`[TRUNK_xx_yy]`. Necessita solo di `LISTEN_PORT=5300` per i propri client SvxLink
locali. Aprire anche la porta `5303` in ingresso sul firewall del reflector
regionale.

---

## Lista di controllo per ogni regione

1. Impostare `LOCAL_PREFIX` al codice a 2 cifre dalla tabella sopra.
2. Aggiungere una sezione `[TRUNK_xx_yy]` per **ciascuna delle altre regioni** (19 sezioni in totale), dove `xx` e `yy` sono la coppia ordinata di codici regionali. I nomi delle sezioni devono essere identici su entrambi i lati del link.
3. Usare lo stesso valore di `SECRET` della sezione corrispondente sul peer — secret
   non corrispondenti impediranno la connessione del trunk.
4. Aprire la porta TCP `5302` in ingresso nel firewall (trunk) e la porta `5300` (client).
5. Se si accettano satellite, aprire anche la porta TCP `5303` in ingresso.
6. Verificare che `HOST` risolva all'IP pubblico del peer dalla rete del server.
