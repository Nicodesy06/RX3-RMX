<!-- SPDX-License-Identifier: MPL-2.0 -->
# RX3-RMX — Dual Sound Color FX e ROLL / LOOP FX per XDJ-RX3

Fork modulare di [rx3-toolkit](https://github.com/Tratosca/rx3-toolkit) (MPL-2.0) che aggiunge due moduli caricati da USB, in RAM, senza scrivere nulla sulla memoria interna:

- **COLOR 2** (`dual-color-fx`): un secondo Sound Color FX per canale, in serie con quello del mixer.
- **ROLL / LOOP FX** (`rmx-looper`): roll in stile RMX-1000 sui pad, sincronizzato al BPM, con HOLD, e un loop live su un bus virtuale indipendente, con OVERDUB a layer, UNDO e CLEAR.

> **Stato al 18 ottobre 2026: implementato, compilato per ARM e verificato solo offline. Nessuna funzione di questo fork è stata eseguita su una RX3 reale.** Nulla qui è pronto per un set dal vivo finché il [piano di test hardware](#7-piano-di-test-su-hardware) non è passato sulla tua console.

---

## 0. Livelli di evidenza

Ogni affermazione in questo documento porta uno di questi marchi.

| Marchio | Significato |
| --- | --- |
| **[U]** | Documentato da upstream (README, REFERENCES.md, commenti nel codice). Upstream dichiara di averlo verificato sul binario `rbp` 1.19, che **non** è nel repository e che non ho potuto leggere. |
| **[C]** | Verificato da me leggendo il codice sorgente di questo repository. |
| **[D]** | Deduzione tecnica mia, non verificata. |
| **[T]** | Verificato da un test automatico offline di questo repository, che passa (compilazione host o ARM, esecuzione su host). |
| **[HW]** | Verificato su RX3 fisica. **Nessuna voce ha questo marchio.** |

Non ho avuto accesso al binario dell'applicazione della console (`rbp`), né all'emulatore privato di upstream, né a una RX3. Ottenere il binario richiede la chiave e il firmware del produttore, previa accettazione dei relativi termini: è una decisione che spetta a te (vedi §9).

---

## 1. Risposte alle 12 domande obbligatorie

**1. Come vengono elaborati i Sound Color FX nella RX3?**
Non è stato determinato. Il repository upstream non contiene alcun lavoro sui Sound Color FX [C]. Ciò che è noto è il contesto: tutto il mixing avviene nell'unico processo C++ `rbp` [U]; il master bus è un buffer `float` stereo che passa per `MicTalkOver` [U][C]; gli effetti Beat FX sono classi software `mixerengine::BeatEffect*` eseguite sulla CPU ARM, al punto che Key Shift istanzia una **seconda copia** dell'effetto stock `BeatEffectPitch` e la esegue sui propri buffer [U][C]. È molto probabile che anche i Color FX siano classi software dello stesso motore [D], ma nome delle classi, indirizzi e punto della catena vanno trovati nel binario.

**2. DSP hardware dedicati o software?**
La SoC è un i.MX 6Quad (4× Cortex-A9, NEON), senza DSP audio dedicato nella SoC [U]. Le prove indicano elaborazione software in `rbp`: master bus in float, Beat FX eseguiti dal codice ARM, effetto Pitch istanziabile da un modulo [U][C]. Che non esista un DSP esterno sulla scheda non è dimostrato, ma nulla nel codice upstream lo suggerisce [D].

**3. Il limite di un solo Color FX è dell'interfaccia, del firmware o dell'hardware?**
Con le prove disponibili: firmware/interfaccia, non hardware [D]. Un processo software che istanzia oggetti effetto può in linea di principio istanziarne due, come dimostra Key Shift con Pitch [U][C]. Che il motore nativo dei Color FX lo permetta senza effetti collaterali (stato globale, buffer condivisi) non è verificato.

**4. È possibile mantenere attive due istanze differenti di effetti esistenti?**
Per i Color FX **nativi**: non verificato, richiede reverse engineering (§9). Per questo ho scelto un percorso che non dipende da quella risposta: il primo effetto resta quello nativo, intatto; il secondo è un motore software nuovo (COLOR 2) [C][T].

**5. Dove si trovano i punti d'inserimento nella pipeline audio?**
Due, già usati da upstream e verificati sul binario 1.19 [U]:
- `dsp::TimeStretch::getStreamAt` (0x000c292c): lo stream di ciascun deck dopo velocità e Master Tempo, **prima** di trim, EQ, Color FX nativo e fader [U][C].
- Il master bus appena prima di `MicTalkOver` (0x0009c750) [U][C]. Se sia prima o dopo il master level e i Beat FX non è documentato [D].

**6. È possibile intercettare e modificare i buffer PCM in tempo reale?**
Sì, sui due punti sopra: Stems mixa nel deck stream e Samples somma nel master bus, con buffer `float` stereo modificabili in place [U][C]. Che queste modifiche suonino correttamente su hardware è dichiarato da upstream "in attesa di collaudo fisico" [U].

**7. È possibile aggiungere un nuovo motore DSP software?**
Sì, a livello di codice: questo fork lo fa [C][T]. Il costo CPU reale sull'A9 non è misurato (§5).

**8. È possibile leggere e modificare lo stato dei pulsanti Sound Color FX e dei LED?**
Non ancora. Il core intercetta `SEND_KEY` (tutti i tasti che passano da lì, con codice, operazione e canale) e scrive i LED dei pad tramite `uif::Led` [U][C]. I codici dei pulsanti Color FX non sono noti e non è noto se passino da `SEND_KEY`; gli ID dei loro LED non sono noti; il valore delle manopole COLOR/PARAMETER non è leggibile da questo codice [C]. COLOR 2 include una **sonda** che registra nel log i codici dei tasti e un mapping configurabile da USB per usarli appena noti (§4.2) [C][T].

**9. È possibile costruire un loop sincronizzato al beat senza interrompere il playback?**
Sì in software: il roll registra dal momento della pressione mentre il brano continua sotto, e al rilascio si torna esattamente al segnale live [T]. Il BPM per deck è letto dall'accessor nativo `PLAY_BPM` già usato da Now Playing, che upstream ha controllato sul binario ma **non osservato su un deck** [U]. La fase della beatgrid non è disponibile: il roll parte all'istante della pressione, non sul beat successivo [C].

**10. La memoria permette buffer circolari e più layer?**
Il modulo prenota circa 28 MB per 3 buffer di roll e 6 di loop (8 beat a 60 BPM, 48 kHz, stereo float) [C], tramite il registro della memoria condiviso di upstream, e si fa da parte se dopo la prenotazione resterebbero meno di 64 MB liberi [C][T]. La RAM totale della RX3 non è documentata upstream; il fatto che Stems accetti pacchetti fino a 512 MiB per deck suggerisce margine ampio [U][D].

**11. È possibile creare un bus audio virtuale indipendente?**
Sì, come somma software nel master bus: il loop vive in buffer propri e suona finché non lo fermi, indipendentemente da deck, fader o cambio traccia [C][T]. Non esiste un bus SEND/RETURN hardware utilizzabile e non lo presumo [C]. L'ingresso AUX non viene toccato.

**12. Cosa potrebbe impedire il funzionamento?**
In ordine di rischio [D]:
1. **Modello di chiamata del deck stream**: roll e FIFO assumono una chiamata per blocco, in ordine. Se `getStreamAt` fosse chiamato più volte per lo stesso audio, il roll accelererebbe. Il modulo misura proprio questo nel log (frame/s per deck, posizioni ripetute).
2. **Thread diversi** per deck e master: gestito con una FIFO single-producer/single-consumer, ma la latenza tra i due è ignota.
3. **CPU**: stimata bassa (§5) ma non misurata; c'è una guardia di sovraccarico.
4. **Semantica del BPM**: BPM originale o dopo il tempo fader? Ignoto; il log lo stampa.
5. **Pulsanti e manopole Color FX**: ignoti finché non si analizza il binario o non si usa la sonda.
6. Le parti di UI del core upstream hanno difetti fisici aperti (ritorno da KEY/STEMS, tab in ritardo) [U].

---

## 2. Tabella di fattibilità

| Funzione | Stato | Motivo e fonte |
| --- | --- | --- |
| **Due Sound Color FX simultanei** | **Realizzato offline**, da verificare su HW | Nativo + COLOR 2 software in serie sul deck stream [C][T]. Punto d'inserimento [U]. |
| Disattivazione individuale | Realizzato offline | Il nativo resta stock; COLOR 2 on/off dal touchscreen o SHIFT+pulsante (dopo la sonda) [C][T]. |
| Entrambi i canali, indipendenti | Realizzato offline | Una riga per deck; effetti diversi per CH1 e CH2 possibili [C][T]. |
| Latenza | Nessuna aggiunta | Elaborazione in place, nessun lookahead [C]. |
| COLOR 2 controllato dalle manopole COLOR/PARAMETER fisiche | **Richiede reverse engineering** | Valore delle manopole non leggibile con gli hook attuali [C]. Oggi: slider touchscreen. |
| Due istanze dei Color FX **nativi** | **Richiede reverse engineering** | Classi e stato nativi sconosciuti; precedente positivo con Pitch [U]. |
| LED dei pulsanti Color FX per due effetti | **Richiede reverse engineering** | ID LED ignoti [C]. |
| Pulsanti Color FX fisici per scegliere COLOR 2 | **Probabilmente realizzabile, da verificare** | Sonda + `buttons.txt` pronti [C][T]; dipende da se passano da `SEND_KEY`. |
| **ROLL 1/16…8 beat** | **Realizzato offline**, da verificare su HW | Senza click, periodico, ritorno esatto al live [T]. |
| Sincronia BPM | Probabilmente realizzabile | Accessor nativo [U] non osservato su deck; fallback 120 BPM [C]. |
| Sincronia alla beatgrid (partenza sul beat) | **Richiede reverse engineering** | Serve la fase della griglia nativa o il parsing ANLZ dal binario/USB [D]. |
| HOLD / LATCH | Realizzato offline | [C][T] |
| Cambio divisione durante il roll | Realizzato offline | Stesso punto di partenza, senza click [T]. |
| **OVERDUB live a layer** | **Realizzato offline** | Fino a 4 layer annullabili, i più vecchi fusi a pezzi senza costi di picco [T]. |
| UNDO ultimo layer / CLEAR | Realizzato offline | [T] |
| Overdub di sample | Realizzato offline, indiretto | Con SOURCE = MST i sample del modulo Samples finiscono nel loop (Samples suona prima nel master bus) [C]. Ma Samples e COLOR 2 sono incompatibili (stesso tab). |
| **Bus virtuale / terzo canale** | **Realizzato offline** | Loop nel master bus, livello e mute propri, indipendente dai deck [C][T]. |
| Sorgente CH1 / CH2 / MASTER | Realizzato offline | CH = dopo roll e COLOR 2, prima del fader; MST = master bus [C]. |
| Continuare il loop cambiando traccia | Realizzato offline | Il loop non dipende dal deck [C]. |
| SEND/RETURN hardware o controlli AUX analogici | **Limitato dall'hardware** | Nessun bus send hardware noto; manopole AUX non leggibili [C][D]. Alternativa: bus software. |
| Pagina touchscreen ROLL / LOOP | Realizzato offline | Tab "RMX", riga a 8 controlli [C][T]; rendering verificato nell'anteprima desktop di upstream [T]. |
| Mappatura sugli 8 pad | Realizzato offline | §4.1 [C][T]. |
| Beat Jump ±32, ripetizione immediata | **Già in upstream** | `beatjump-32bars`, `beatjump-no-quantize` [U]; riusati, non modificati. |
| Beat Jump ±16 | **Richiede reverse engineering** | Stessa tecnica di ±32 (patch guardata del binario) ma servono i byte del binario [U][D]. |
| Key Shift ±12, per deck | **Già in upstream** | `keyshift` [U]. |
| Key Sync con accetta/rifiuta | **Già in upstream** | `key-sync` propone, il DJ applica [U]. |

Nessuna funzione richiesta risulta impossibile con l'architettura attuale; i limiti veri sono l'assenza di un bus send hardware e, finché non si analizza il binario, il controllo dei pulsanti/manopole Color FX nativi.

---

## 3. Architettura

### 3.1 Pipeline audio

```text
           deck stream (TimeStretch::getStreamAt), per deck, in ordine di priorità
 ┌──────┐   ┌───────┐  ┌──────────┐  ┌──────────┐  ┌─────────────┐
 │ deck ├──►│ Stems ├─►│  ROLL    ├─►│ COLOR 2  ├─►│ tap loop CH ├──► mixer nativo:
 └──────┘   │  (50) │  │ deck (55)│  │   (60)   │  │    (65)     │    trim, EQ, Color FX
            └───────┘  └──────────┘  └──────────┘  └──────┬──────┘    nativo, fader, xfader
                                                          │ FIFO SPSC
                                                          ▼
 master bus (prima di MicTalkOver)                ┌──────────────┐
 mix nativo ─►│ Samples (50) │─►│ ROLL master │──►│ tap MST /    │──► + loop × livello ──► talkover ──► uscite
              └──────────────┘  │    (60)     │   │ LOOP bus (60)│
                                └─────────────┘   └──────────────┘
```

- Il core upstream permetteva **un solo** client per stage. Questo fork lo estende a una **catena di 4 client ordinati per priorità** sullo stesso buffer, pubblicata con doppia tabella + drain, così il thread audio non vede mai una lista a metà [C][T]. Stems e Samples non sono cambiati.
- COLOR 2 sta **dopo** il roll: puoi filtrare o mandare in eco un roll. Il Color FX nativo sta dopo entrambi, nel mixer.
- Il loop registra **dopo** roll e COLOR 2, così si può fare roll → overdub, il gesto tipico dell'RMX.
- Con SOURCE = MST la copia della sorgente si fa **prima** di sommare il loop, quindi il loop non si registra mai dentro se stesso [C].

### 3.2 ROLL (`rx3_fx_rmx.h`, `fx_roll_*`)

- **Pressione**: si registra dal campione corrente; il primo passaggio *è* il segnale live, quindi l'attacco non può fare click [T].
- **Giunzione**: i 256 frame dopo la fine della slice vengono incrociati con l'inizio (crossfade lineare complementare, 5,8 ms a 44,1 kHz); gli originali dell'inizio vengono conservati per ricalcolare la giunzione se cambi divisione [T].
- **Registrazione continua** durante il roll: una divisione più lunga scelta a metà roll riparte dallo stesso punto, se il materiale c'è [C][T].
- **Salti di lettura** (divisione più corta) nascosti da un "dip" di 32+16 frame [T].
- **Rilascio**: rampa di 220 frame (5 ms) verso il live, che non si è mai fermato [T].
- Richieste dal thread input in **una sola parola atomica** (stato, lunghezza, numero di pressione); solo il thread audio cambia lo stato [C].

### 3.3 LOOP e OVERDUB (`fx_loop_*`)

- **Registrazione**: una divisione (minimo 1/4 di beat) dalla SOURCE; la prima ripetizione entra in dissolvenza sulla giunzione [T].
- **Layer**: ogni sessione di OVERDUB scrive in uno slot proprio, che ricorda l'intervallo circolare che ha scritto: un layer corto non fa riemergere dati vecchi [C][T]. Il primo giro sovrascrive, i successivi sommano; ingresso e uscita della sessione con rampa di 64 frame [T].
- **UNDO**: dissolvenza dell'ultimo layer in 5 ms, poi lo slot torna libero [T]. Quando gli slot annullabili (4) sono pieni, il più vecchio viene **fuso nella base a pezzi da 4096 frame per blocco**: ogni frame passa dal layer alla base tra due letture, quindi la riproduzione non lo sente mai due volte né zero volte [T].
- **STOP/PLAY** mantiene la fase (il cursore continua a girare in silenzio); **CLEAR** svuota dopo una dissolvenza [C][T].

### 3.4 COLOR 2 (`rx3_fx_color.h`)

Sei motori scritti da zero sulla descrizione sonora degli effetti, **non** copiati dal firmware [C]:

| Effetto | Sinistra | Destra | PARAMETER | Implementazione |
| --- | --- | --- | --- | --- |
| FILTER | LPF 20 kHz→~60 Hz | HPF 20 Hz→~10 kHz | risonanza | SVF trapezoidale (stabile in modulazione) |
| DUB ECHO | eco più scura | eco più sottile | feedback | delay a ½ beat, filtro nel feedback, coda che resta |
| SPACE | riverbero scuro | riverbero brillante | decadimento | FDN a 4 linee (Hadamard), smorzamento |
| CRUSH | bit/sample-rate, scuro | idem, sottile | tono | quantizzazione 16→3 bit, hold 1→24 frame |
| NOISE | rumore LPF | rumore HPF | livello | rumore bianco filtrato sommato |
| SWEEP | gate a tempo | band-pass che scorre | velocità gate / banda | gate sincronizzato, BPF normalizzato |

- Centro = bypass **bit-exact** su tutti e sei [T]; zona morta del 3% come il detent di una manopola.
- Controlli smussati (~5 ms), coefficienti ricalcolati ogni 32 frame [C].
- Un motore per tipo e per deck: cambiando effetto, la coda di DUB ECHO o SPACE continua a suonare sotto il nuovo [T].
- Misure offline: FILTER tutto a sinistra −78 dB a 5 kHz; tutto a destra −84 dB a 100 Hz; prima eco esattamente a ½ beat (11 025 frame a 120 BPM); coda di SPACE che decade di oltre 70 dB in 2 s; tutti stabili e limitati per 10 s a impostazioni massime su rumore forte [T].

### 3.5 Interazione con il mixer originale

- Nessuna funzione stock viene disattivata: Color FX nativo, Beat FX, EQ, fader e AUX restano come sono [C].
- COLOR 2 e ROLL di canale stanno **prima** del fader: il fader, l'EQ e il Color FX nativo agiscono anche su di loro.
- Il loop esce sul master **dopo** fader e crossfader, con livello proprio: è il "terzo canale".
- Con SOURCE CH1/CH2 il loop ascolta il deck **prima** del fader: puoi campionare un deck a fader chiuso (come un send pre-fader).

---

## 4. Controlli ed ergonomia

### 4.1 ROLL / LOOP — tab **RMX** (dove starebbe STEMS)

Con il pannello aperto i pad di **entrambi** i deck appartengono a ROLL / LOOP:

| Pad | Tenuto | SHIFT + pad |
| --- | --- | --- |
| 1 | roll 1/16 | HOLD |
| 2 | roll 1/8 | LOOP: REC → STOP / PLAY |
| 3 | roll 1/4 | OVERDUB |
| 4 | roll 1/2 | UNDO (poi CLEAR) |
| 5 | roll 1 beat | CLEAR |
| 6 | roll 2 | SOURCE CH1 → CH2 → MST |
| 7 | roll 4 | livello −10 |
| 8 | roll 8 | livello +10 |

I pad di un deck fanno il roll **di quel deck**; con SOURCE = MST fanno il roll del master. LED: divisioni in sfumatura dal ciano (corte) al magenta (lunghe), quella attiva accesa piena, lampeggiante se in HOLD; con SHIFT premuto i pad mostrano le funzioni del loop (REC lampeggia rosso in registrazione, OVERDUB rosso, ecc.).

Riga touchscreen, su tutta la larghezza:

```text
[< CH1 >] [< 1/4 >] [ROLL] [HOLD] [REC] [OVERDUB] [UNDO] [LVL 80]
```

ROLL al tocco = roll bloccato della SOURCE; REC diventa `REC *`, poi STOP/PLAY; UNDO diventa CLEAR quando resta solo la base; LVL: tocco = mute, tieni e trascina = livello.

### 4.2 COLOR 2 — tab **COLOR 2** (posizione STATUS)

```text
[<  DUB ECHO  >] [COLOR +40] [PARAM 50]      una riga per deck
```

- `<` `>` scelgono l'effetto; il **nome** accende/spegne.
- **COLOR**: tieni e trascina (bipolare); **un tocco lo riporta al centro** — l'uscita più rapida da un effetto a metà mix. **PARAM**: tieni e trascina; un tocco lo riporta a 50.
- Ogni effetto ricorda il proprio COLOR e PARAMETER (la tua proposta "ultimo valore per effetto").
- **SHIFT + pulsante Color FX** (solo dopo aver scritto `buttons.txt` dalla sonda): aggiunge/toglie quell'effetto come COLOR 2 su entrambi i canali, come fa il mixer con la selezione; quella pressione non arriva al mixer. Senza SHIFT i pulsanti restano stock.

Proposta per quando le manopole saranno leggibili (richiede RE): la manopola COLOR fisica pilota l'ultimo effetto toccato; l'altro resta congelato al suo valore con "soft takeover" (riprende il controllo solo quando la manopola ripassa per il valore memorizzato), per evitare salti.

---

## 5. Prestazioni, memoria, latenza

| Voce | Valore | Evidenza |
| --- | --- | --- |
| Latenza aggiunta da ROLL e COLOR 2 | 0 campioni | in place, nessun lookahead [C] |
| Latenza sorgente CH → loop | dimensione della FIFO al momento, ignota | dipende dal modello di thread [D]; il log riporta underrun e salti |
| Memoria ROLL / LOOP | ~27,7 MB (9 × 3,08 MB) | [C] |
| Memoria COLOR 2 | ~1,6 MB (4 × 393 KB) | [C] |
| Costo host, ns/frame (Xeon 2,1 GHz, -O2) | FILTER 12, CRUSH 13, NOISE 12, SWEEP 11, SPACE 19, DUB ECHO 26, ROLL 3, LOOP (4 layer + overdub) 17 | [T] |
| Stima Cortex-A9 | ×10–20 rispetto all'host → caso peggiore (2 deck × 2 motori COLOR 2 + 3 roll + loop) ≈ 1–3 µs/frame ≈ **5–13 % di un core** | **stima** [D], non misurata |

Protezioni: guardia di sovraccarico (se il blocco master supera metà della sua durata per 16 blocchi di fila, il modulo si bypassa fino al riavvio) [C]; buffer prenotati e toccati all'avvio, così il thread audio non prende page fault [C]; nessuna allocazione, I/O o attesa nei thread audio e input [C]; log diagnostico limitato per non saturare il log condiviso (128 KiB) [C].

---

## 6. Build e test offline

Prerequisiti: Python 3.12+, Clang + LLD, `pip install -r requirements.txt`, `hexdump` (per i test della safe mode).

```sh
make hook        # cross-compila librx3_core.so (ARM EABI5) con tutti i moduli
make test        # 464 test: upstream + questo fork
make preflight   # controllo dei file pubblicabili
python3 -m app.preview --out build/pad-preview   # anteprima delle righe touchscreen
```

Test specifici di questo fork:

| Test | Cosa verifica |
| --- | --- |
| `tests/fx/fx_engine_test.c` | motori: roll senza click e periodico, ritorno esatto al live, cambi di divisione, blocchi 64/512/333; loop, overdub, undo, merge, clear; bypass bit-exact, filtri, tempo dell'eco, coda del riverbero, stabilità; costi |
| `tests/test_performance_fx.py` | moduli attraverso i loro callback con servizi mock: pad → roll misurato sull'audio, HOLD, SOURCE, loop/overdub/undo/clear via deck+master, LED; COLOR 2 per deck, bypass, coda attraverso il cambio, memoria per effetto, SHIFT+pulsante, sonda, parsing di `buttons.txt`; hook `module.sh` in sandbox (anche contro injection) |
| `tests/test_services.py` | catena audio: ordine per priorità, limite, un posto per owner, distacco solo con l'ultimo client |
| `tests/test_runtime_transitions.py` | immagini dei tab dichiarate dalla riga |

Generare il payload USB (serve la **tua** chiave, fuori dal repository):

```sh
python3 -m app.runtime.cli build --firmware 1.20 \
  --patch dual-color-fx --patch rmx-looper \
  --patch keyshift --patch key-sync --patch beatjump-32bars \
  --key /percorso/della/tua/chiave --output /Volumes/TUA_USB
```

Ho verificato questa pipeline con una chiave **fittizia**: produce un `autoexec.bin` valido come ISO, con i moduli e gli asset attesi, e rifiuta le combinazioni incompatibili (`rmx-looper` + `stems`, `dual-color-fx` + `samples`) [T]. Un'immagine con chiave fittizia non viene accettata dalla console ed è stata cancellata. Anche l'app desktop di upstream (`make app`) mostra i due moduli nella categoria *Performance FX*.

Opzioni sulla chiavetta:

| File | Effetto |
| --- | --- |
| `RX3_RUNTIME/dual-color-fx/probe` (vuoto) | attiva la sonda: i codici dei tasti finiscono nel log di sessione |
| `RX3_RUNTIME/dual-color-fx/buttons.txt` | sei codici esadecimali (SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER) per SHIFT + pulsante |

---

## 7. Piano di test su hardware

Da eseguire nell'ordine, a casa, con una chiavetta di prova. Attiva **Troubleshooting report** nell'app. Fermati al primo fallimento e conserva `RX3_RUNTIME/session.txt`.

| # | Prova | Atteso | Cosa guardare nel log |
| --- | --- | --- | --- |
| H1 | Avvio con solo `rmx-looper` + `dual-color-fx` | interfaccia normale, tab RMX e COLOR 2 visibili | `module active: rmx-looper`, `module active: dual-color-fx`, nessun `refused` |
| H2 | Riproduci un brano su deck 1 per 60 s senza toccare nulla | audio identico allo stock | `rmx deck 1 frames` ≈ 44 100 × `rmx report ms`/1000 (≈ 220 500 ogni 5 s); `repeated positions` ≈ 0; `rmx master frames` uguale |
| H3 | BPM | `rmx deck 1 bpm x100` = BPM mostrato × 100; poi sposta il tempo fader | scopri se segue il tempo fader |
| H4 | Roll 1/4 su deck 1, tenuto 8 beat, poi rilascio | ripetizione a tempo, nessun click, ritorno al punto giusto del brano | `rmx worst us` molto sotto la durata del blocco |
| H5 | HOLD, cambio divisione, roll su MST | come da guida | — |
| H6 | LOOP da CH1, 1 beat, poi cambia traccia sul deck 1 | il loop continua | `rmx fifo 1 underruns/skips` stabili |
| H7 | OVERDUB × 6, UNDO, CLEAR | layer udibili, undo rimuove l'ultimo, clear silenzia | — |
| H8 | COLOR 2 FILTER su CH1 + FILTER nativo su CH1 | due filtri in serie; CH2 intatto | — |
| H9 | COLOR 2 DUB ECHO + nativo CRUSH; poi COLOR 2 → FILTER | l'eco suona ancora mentre entra il filtro | — |
| H10 | Sonda: file `probe`, premi i 6 pulsanti Color FX in ordine | righe `fx2 probe key=0x…` | se i pulsanti **non** compaiono, non passano da `SEND_KEY` |
| H11 | `buttons.txt` dalla sonda, SHIFT + DUB ECHO | COLOR 2 = DUB ECHO su entrambi i canali; la selezione nativa non cambia | — |
| H12 | Stress 60 min: due deck, roll frequenti, loop con overdub, COLOR 2 sempre attivo | nessun dropout, nessun crash | nessun `overload guard`; `rmx worst us` stabile |
| H13 | Ritorno allo stock | spegni, togli la chiavetta, riaccendi: console stock | — |

Solo dopo H1–H13 superati una funzione può essere considerata per un set, e comunque prima con una chiavetta stock di riserva.

---

## 8. Limiti noti

- Nessun test su hardware (§7).
- COLOR 2 non segue le manopole fisiche COLOR/PARAMETER; i LED dei pulsanti Color FX non mostrano COLOR 2.
- Roll non quantizzato alla beatgrid; loop di lunghezza fissa dopo la registrazione.
- BPM da un accessor non osservato su deck; fallback 120 BPM (roll) o ultimo tempo (COLOR 2).
- `rmx-looper` non è compatibile con Stems; `dual-color-fx` non è compatibile con Samples (stesso tab touchscreen). Upstream ha solo tre posizioni di pannello.
- I LED dei pad si aggiornano quando li aggiorna il player.
- Le etichette "RMX", "ROLL / LOOP" e "COLOR 2" dei tab sono composte in Liberation Sans, un'approssimazione del carattere del firmware (licenziato, non redistribuibile).
- Il limite delle immagini private è salito da 0x16a5 a 0x16b5: reinserire questa chiavetta in una console che ha già in RAM una versione precedente del mod viene rifiutato; spegni e riaccendi.

### Ritorno allo stock

Stop → spegni → togli la chiavetta → accendi. Niente viene scritto nella memoria interna. Per saltare il mod a un inserimento, tieni SHIFT mentre inserisci la chiavetta. Per disattivare un solo modulo: deselezionalo nell'app, oppure crea `/tmp/rx3-rmx-looper.off` o `/tmp/rx3-dual-color-fx.off` (shell diagnostica).

---

## 9. Reverse engineering che resta (richiede il binario `rbp`)

Il binario non è strippato [U], quindi i nomi C++ sono leggibili. Con `rbp` in mano, nell'ordine:

1. `python3 scripts/check_addresses.py rbp` — conferma che tutti gli indirizzi e i guard di questo fork (inclusi `RMX_PLAY_BPM` e `FX2_PLAY_BPM`) corrispondono al tuo firmware.
2. `python3 scripts/find_fx_symbols.py rbp > fx-symbols.txt` — elenca le classi Color FX, mixer, LED, beatgrid e beat jump.
3. Dalle classi Color FX: ctor/initialize/execute e `adjustParameter` (come per Pitch) → seconda istanza **nativa**; il campo del valore di COLOR/PARAMETER → manopole fisiche per COLOR 2; gli ID LED → luci dei due effetti.
4. Fase della beatgrid del deck → roll quantizzato.
5. Tabella del Beat Jump → ±16 con lo stesso metodo di ±32.

Ottenere `rbp` richiede la chiave del produttore e il pacchetto firmware, previa accettazione dei termini; upstream documenta la procedura (`REFERENCES.md`, *Getting a root filesystem from the published GPL sources*) e la sua posizione legale (`LEGAL.md`, Direttiva 2009/24/CE artt. 5(3) e 6). Non l'ho fatto in questa sessione perché è una decisione tua; nessun file del produttore va mai committato.

---

## 10. RMX-1000: cosa è stato preso, e cosa no

Fonti pubbliche consultate: recensioni e schede prodotto ([DJWORX](https://djworx.com/review-pioneer-rmx-1000-remix-station/), [Pioneer DJ store](https://pioneerdjstore.com/products/rmx-1000)); il manuale ufficiale non era disponibile nella ricerca, quindi i punti sotto sono da confermare sul manuale.

- **X-PAD FX ROLL** sull'RMX ripete fette del segnale in ingresso a divisioni ritmiche; **HOLD** le mantiene dopo aver tolto il dito. → Modalità A di questo fork (ROLL + HOLD).
- **OVERDUB** sull'RMX, secondo le fonti, registra *pattern* di pad (i sample KICK/SNARE/CLAP/HI-HAT) in un loop, **non** l'audio live: è un sequencer di sample, non un looper audio. → Modalità B di questo fork è invece un **looper audio live** a layer, che è ciò che hai chiesto; l'overdub di sample è possibile solo indirettamente (SOURCE = MST con Samples).
- L'RMX ha anche SCENE FX, ISOLATE FX e un'uscita dedicata; qui non replicati.
