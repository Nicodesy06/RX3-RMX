<!-- SPDX-License-Identifier: MPL-2.0 -->
# RX3-RMX — RMX X-PAD, COLOR 2 e ISOLATOR per XDJ-RX3

Fork modulare di [rx3-toolkit](https://github.com/Tratosca/rx3-toolkit) (MPL-2.0) che aggiunge tre moduli caricati da USB, in RAM, senza scrivere nulla sulla memoria interna:

- **RMX X-PAD** (`rmx-looper`): la sezione X-PAD FX dell'RMX-1000 sul touchscreen e sui pad. ROLL, KICK, SNARE, CLAP e HI HAT su una striscia da 1/8 a 2 beat da percorrere col dito, con HOLD, OVERDUB, PITCH e VOLUME.
- **COLOR 2** (`dual-color-fx`): due Sound Color FX insieme. Tieni premuti due dei sei pulsanti Color FX fisici: il primo è quello nativo del mixer, il secondo un motore software in serie. Nessuna interfaccia sullo schermo.
- **ISOLATOR** (`isolator`): isolator a tre bande per deck sul touchscreen (LOW, MID, HIGH da kill a +6 dB). Ha anche i pulsanti FX LOW / MID / HI, che scelgono su quali bande agiscono COLOR 2 e i roll, come i pulsanti di banda dei Beat FX del DJM-900NXS2.

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

Non ho avuto accesso:
- al binario dell'applicazione della console (`rbp`);
- all'emulatore privato di upstream;
- a una RX3.

Ottenere il binario richiede la chiave e il firmware del produttore, previa accettazione dei relativi termini: è una decisione che spetta a te (vedi §9).

---

## 1. Risposte alle 12 domande obbligatorie

**1. Come vengono elaborati i Sound Color FX nella RX3?**
Non è stato determinato: il repository upstream non contiene alcun lavoro sui Sound Color FX [C]. È noto il contesto:
- tutto il mixing avviene nell'unico processo C++ `rbp` [U];
- il master bus è un buffer `float` stereo che passa per `MicTalkOver` [U][C];
- i Beat FX sono classi software `mixerengine::BeatEffect*` eseguite sulla CPU ARM. Key Shift arriva a istanziare una **seconda copia** dell'effetto stock `BeatEffectPitch` e la esegue sui propri buffer [U][C].

È molto probabile che anche i Color FX siano classi software dello stesso motore [D]. Nomi delle classi, indirizzi e punto della catena vanno però trovati nel binario.

**2. DSP hardware dedicati o software?**
La SoC è un i.MX 6Quad (4× Cortex-A9, NEON), senza DSP audio dedicato [U]. Le prove indicano elaborazione software in `rbp`: master bus in float, Beat FX eseguiti dal codice ARM, effetto Pitch istanziabile da un modulo [U][C]. Non è dimostrato che sulla scheda manchi un DSP esterno, ma nulla nel codice upstream lo suggerisce [D].

**3. Il limite di un solo Color FX è dell'interfaccia, del firmware o dell'hardware?**
Con le prove disponibili: firmware/interfaccia, non hardware [D]. Un processo software che istanzia oggetti effetto può in linea di principio istanziarne due, come dimostra Key Shift con Pitch [U][C]. Non è verificato che il motore nativo dei Color FX lo permetta senza effetti collaterali (stato globale, buffer condivisi).

**4. È possibile mantenere attive due istanze differenti di effetti esistenti?**
Per i Color FX **nativi**: non verificato, richiede reverse engineering (§9). Per questo COLOR 2 non dipende da quella risposta. Il primo effetto resta quello nativo, intatto; il secondo è un motore software nuovo, nello stesso gesto: due pulsanti tenuti insieme [C][T].

**5. Dove si trovano i punti d'inserimento nella pipeline audio?**
Due, già usati da upstream e verificati sul binario 1.19 [U]:
- `dsp::TimeStretch::getStreamAt` (0x000c292c): lo stream di ciascun deck dopo velocità e Master Tempo, **prima** di trim, EQ, Color FX nativo e fader [U][C].
- Il master bus appena prima di `MicTalkOver` (0x0009c750) [U][C]. Non è documentato se sia prima o dopo il master level e i Beat FX [D].

**6. È possibile intercettare e modificare i buffer PCM in tempo reale?**
Sì, nei due punti sopra: Stems mixa nel deck stream e Samples somma nel master bus, con buffer `float` stereo modificabili in place [U][C]. Upstream dichiara che il suono corretto su hardware è "in attesa di collaudo fisico" [U].

**7. È possibile aggiungere un nuovo motore DSP software?**
Sì, a livello di codice: questo fork ne aggiunge diversi (roll, X-PAD, sei Color FX, isolator a tre bande) [C][T]. Il costo CPU reale sull'A9 non è misurato (§5).

**8. È possibile leggere e modificare lo stato dei pulsanti Sound Color FX e dei LED?**
Non ancora. Cosa fa già il core:
- intercetta `SEND_KEY`, cioè tutti i tasti che passano da lì, con codice, operazione, canale e ora anche i tre argomenti successivi [U][C];
- scrive i LED tramite `uif::Led`, e un modulo può ora filtrare quelle scritture [C][T].

Cosa non è noto [C]:
- i codici dei pulsanti Color FX, e se passino da `SEND_KEY`;
- gli ID dei loro LED;
- il valore delle manopole COLOR/PARAMETER e della manopola AUX.

COLOR 2 include una **sonda** che registra nel log codici, argomenti e ID LED. C'è anche un mapping da USB (`buttons.txt`) per usarli appena noti (§4.2) [C][T].

**9. È possibile costruire un loop sincronizzato al beat senza interrompere il playback?**
Sì in software. Il roll registra dal momento del tocco mentre il brano continua sotto, e al rilascio si torna esattamente al segnale live [T]. Il pattern OVERDUB dura 4 beat dal primo colpo [T].

Il BPM per deck viene dall'accessor nativo `PLAY_BPM` già usato da Now Playing. Upstream lo ha controllato sul binario ma **non osservato su un deck** [U].

La fase della beatgrid non è disponibile: roll e pattern partono all'istante del tocco, non sul beat successivo [C].

**10. La memoria permette buffer circolari e più layer?**
L'X-PAD prenota circa 18 MB [C]:
- 3 buffer di roll da 8 beat a 60 BPM;
- 5 tracce di pattern da 4 beat;
- 4 suoni sintetizzati.

Usa il registro della memoria condiviso di upstream. Se dopo la prenotazione resterebbero meno di 64 MB liberi, il modulo si fa da parte [C][T].

La RAM totale della RX3 non è documentata upstream. Il fatto che Stems accetti pacchetti fino a 512 MiB per deck suggerisce un margine ampio [U][D].

**11. È possibile creare un bus audio virtuale indipendente?**
Sì, come somma software nel master bus: suoni e pattern dell'X-PAD vivono in buffer propri, con volume proprio, indipendenti da deck, fader e cambio traccia [C][T]. Non esiste un bus SEND/RETURN hardware utilizzabile e non lo presumo [C]. L'ingresso AUX non viene toccato.

**12. Cosa potrebbe impedire il funzionamento?**
In ordine di rischio [D]:
1. **Modello di chiamata del deck stream.** Roll e FIFO assumono una chiamata per blocco, in ordine. Se `getStreamAt` fosse chiamato più volte per lo stesso audio, il roll accelererebbe. Il modulo misura proprio questo nel log (frame/s per deck, posizioni ripetute).
2. **Thread diversi** per deck e master. È gestito con una FIFO single-producer/single-consumer, ma la latenza tra i due è ignota.
3. **CPU**: stimata bassa (§5) ma non misurata; c'è una guardia di sovraccarico.
4. **Semantica del BPM**: BPM originale o dopo il tempo fader? Ignoto; il log lo stampa.
5. **Pulsanti e manopole Color FX, manopola AUX**: ignoti finché non si usa la sonda o non si analizza il binario. Senza i codici dei pulsanti, COLOR 2 non ha modo di essere attivato.
6. **Touchscreen**: lo strato hookato riporta un solo tocco, quindi la striscia segue un dito [C].
7. Le parti di UI del core upstream hanno difetti fisici aperti (ritorno da KEY/STEMS, tab in ritardo) [U].

---

## 2. Tabella di fattibilità

| Funzione | Stato | Motivo e fonte |
| --- | --- | --- |
| **Due Sound Color FX simultanei** | **Realizzato offline**, da verificare su HW | Nativo + COLOR 2 software in serie sul deck stream [C][T]. Punto d'inserimento [U]. |
| Attivazione con due pulsanti Color FX tenuti insieme | **Realizzato offline, dipende dalla sonda** | Accordo: la seconda pressione non arriva al mixer [C][T]. Servono i codici dei pulsanti e che passino da `SEND_KEY` [C]. |
| Disattivazione individuale | Realizzato offline | Stesso accordo di nuovo, o tocco del secondo pulsante da solo; il nativo resta stock [C][T]. |
| LED dei pulsanti per i due effetti | **Pronto, dipende dalla sonda** | Filtro LED nel core e riga `leds=` [C][T]; ID LED ignoti. |
| Latenza | Nessuna aggiunta | Elaborazione in place, nessun lookahead [C]. |
| COLOR 2 guidato dalle manopole COLOR/PARAMETER fisiche | **Richiede reverse engineering** | Valore non leggibile con gli hook attuali [C]. Oggi: profondità fissa da `depth=`. La sonda registra gli argomenti di `SEND_KEY`, dove potrebbe comparire. |
| Due istanze dei Color FX **nativi** | **Richiede reverse engineering** | Classi e stato nativi sconosciuti; precedente positivo con Pitch [U]. |
| **X-PAD: ROLL + 4 strumenti, striscia a 5 sezioni** | **Realizzato offline**, da verificare su HW | KICK, SNARE, CLAP, HI HAT sintetizzati o WAV da USB; 1/8…2/1, un'ottava più fine con SHIFT [C][T]. |
| Scorrimento del dito sulla striscia | Realizzato offline | Widget STRIP nel core: tocco, trascinamento tra sezioni, rilascio [C][T]. |
| Sincronia BPM | Probabilmente realizzabile | Accessor nativo [U] non osservato su deck; fallback 120 BPM [C]. |
| Sincronia alla beatgrid (partenza sul beat) | **Richiede reverse engineering** | Serve la fase della griglia nativa o il parsing ANLZ dal binario/USB [D]. |
| HOLD | Realizzato offline | [C][T] |
| **OVERDUB come sull'RMX** | **Realizzato offline** | Standby → il primo colpo avvia un pattern di 4 beat; una traccia per suono + ROLL; mute (HOLD+suono), cancella (OVERDUB+suono); OVERDUB di nuovo = stop e cancella tutto [T]. |
| **PITCH** di suoni e roll | Realizzato offline | ±12 semitoni, come la manopola PITCH dell'RMX; il roll mantiene la lunghezza [T]. Il tono del brano resta a Key Shift [U]. |
| **VOLUME** dell'X-PAD sul touchscreen | Realizzato offline | 0–100, tocco = mute [C][T]. |
| VOLUME sulla manopola AUX (terzo canale) | **Richiede reverse engineering** | Valore della manopola non leggibile oggi; la sonda è il primo passo [C]. |
| **ISOLATOR LOW/MID/HIGH per deck** | **Realizzato offline** | LR4 a 250 Hz / 2,5 kHz allineato in fase, somma piatta; kill … +6 dB; tutto a 0 dB = bypass bit-exact [T]. |
| **Bande degli effetti (stile DJM-900NXS2)** | **Realizzato offline per gli effetti di questo fork** | FX LOW/MID/HI applicano COLOR 2 e i roll di deck solo alle bande scelte [T]. |
| Bande dei Beat FX **nativi** | **Richiede reverse engineering** | I Beat FX sono nel firmware; il loro instradamento non è noto [U][D]. |
| **Bus virtuale / terzo canale** | **Realizzato offline** | Suoni e pattern sommati nel master bus, volume e mute propri [C][T]. |
| Sorgente CH1 / CH2 / MASTER del roll | Realizzato offline | Pad 8 o tocco su ROLL [C][T]. |
| SEND/RETURN hardware o controlli AUX analogici | **Limitato dall'hardware** | Nessun bus send hardware noto [C][D]. Alternativa: bus software. |
| Beat Jump ±32, ripetizione immediata | **Già in upstream** | `beatjump-32bars`, `beatjump-no-quantize` [U]; riusati, non modificati. |
| Beat Jump ±16 | **Richiede reverse engineering** | Stessa tecnica di ±32 (patch guardata del binario), ma servono i byte del binario [U][D]. |
| Key Shift ±12, per deck | **Già in upstream** | `keyshift` [U]. |
| Key Sync con accetta/rifiuta | **Già in upstream** | `key-sync` propone, il DJ applica [U]. |

Nessuna funzione richiesta risulta impossibile con l'architettura attuale. I limiti veri sono due. Il primo è l'assenza di un bus send hardware. Il secondo vale finché non si fanno la sonda o l'analisi del binario: pulsanti, manopole e LED nativi.

---

## 3. Architettura

### 3.1 Pipeline audio

```text
             deck stream (TimeStretch::getStreamAt), per deck, in ordine di priorità
 ┌──────┐   ┌───────┐  ┌───────────┐  ┌──────────┐  ┌──────────┐  ┌───────────┐
 │ deck ├──►│ Stems ├─►│ ROLL deck ├─►│ COLOR 2  ├─►│ tap roll ├─►│ ISOLATOR  ├──► mixer nativo:
 └──────┘   │  (50) │  │ (55) sulle│  │ (60)sulle│  │   (65)   │  │   (70)    │    trim, EQ, Color FX
            └───────┘  │ bande FX  │  │ bande FX │  └────┬─────┘  └───────────┘    nativo, fader, xfader
                       └───────────┘  └──────────┘       │ FIFO SPSC
                                                         ▼
 master bus (prima di MicTalkOver)
 mix nativo ─►│ Samples (50) │─►│ ROLL master + X-PAD (60): suoni, ripetizioni, pattern × VOL │──► talkover ──► uscite
```

- Il core upstream permetteva **un solo** client per stage. Questo fork lo estende a una **catena di 4 client ordinati per priorità** sullo stesso buffer, pubblicata con doppia tabella + drain, così il thread audio non vede mai una lista a metà [C][T]. Stems e Samples non sono cambiati.
- COLOR 2 sta **dopo** il roll: puoi filtrare o mandare in eco un roll. Il Color FX nativo sta dopo entrambi, nel mixer.
- L'OVERDUB riceve il roll del deck dopo COLOR 2, tramite FIFO, e i suoni dal master: roll → overdub, il gesto tipico dell'RMX.
- ISOLATOR è l'ultimo client del deck: agisce su tutto ciò che questo mod fa al deck, prima di trim ed EQ nativi.

### 3.2 Servizio bande (core API 20)

- ISOLATOR **fornisce** una maschera di bande per deck (`rx3_band_api.h`); COLOR 2 e l'X-PAD la **leggono**. Senza ISOLATOR la maschera è "tutte le bande" [C][T].
- `fx_band_insert` (`rx3_fx_bands.h`) divide il segnale in tre bande LR4 (250 Hz, 2,5 kHz, con allpass di allineamento), applica l'effetto solo a quelle scelte e risomma [T].
- Con tutte le bande l'effetto agisce sul segnale intero, bit-exact rispetto a senza split [T].
- Il percorso cambia solo quando l'effetto è inattivo, con un crossfade di 220 frame (5 ms), quindi niente click [T].
- Misura offline: effetto solo su HIGH → 80 Hz a −0,09 dB, 9 kHz a −49,5 dB; somma delle bande piatta entro 0,001 dB [T].

### 3.3 ROLL (`rx3_fx_rmx.h`, `fx_roll_*`)

- **Tocco**: si registra dal campione corrente. Il primo passaggio *è* il segnale live, quindi l'attacco non può fare click [T].
- **Giunzione**: i 256 frame dopo la fine della slice vengono incrociati con l'inizio, con un crossfade lineare complementare di 5,8 ms a 44,1 kHz [T].
- **Scorrimento** su un'altra sezione: stessa partenza, nuova lunghezza, come sull'RMX [C][T].
- **Salti di lettura** (sezione più corta) nascosti da un "dip" di 32+16 frame [T].
- **Rilascio**: rampa di 220 frame (5 ms) verso il live, che non si è mai fermato [T].
- **PITCH**: varispeed sulla slice con ripartenza a ogni periodo e rampe di 64 frame. Il periodo resta quello del beat, quindi il roll resta a tempo [T].
- Le richieste arrivano dal thread input in **una sola parola atomica**; solo il thread audio cambia lo stato [C].

### 3.4 X-PAD (`rx3_fx_xpad.h`)

- **Suoni**: KICK, SNARE, CLAP e HI HAT sintetizzati (0,5 s), oppure WAV dalla chiavetta [C].
  - Misura: KICK con il 97 % dell'energia sotto 250 Hz, HI HAT con il 99 % sopra 2,5 kHz [T].
  - Lo stesso suono ripetuto taglia il precedente (choke), come un tamburo [T].
- **Ripetizione sulla striscia**: il blocco audio viene diviso al frame esatto di ogni colpo, quindi la ripetizione è a tempo anche con blocchi di lunghezza qualsiasi [T].
- **OVERDUB**:
  - standby; il primo colpo avvia un pattern di 4 beat;
  - una traccia per suono più una per il ROLL;
  - ogni traccia viene suonata prima di essere scritta, così un suono registrato ora torna al giro dopo, senza raddoppio;
  - la cancellazione svuota la traccia a pezzi, senza picchi di costo [T].
- **VOL** con legge da fader (quadratica) e transizione morbida [C].

### 3.5 COLOR 2 (`rx3_fx_color.h`)

Sei motori scritti da zero sulla descrizione sonora degli effetti, **non** copiati dal firmware [C]:

| Effetto | Implementazione |
| --- | --- |
| FILTER | SVF trapezoidale (stabile in modulazione), LPF/HPF con risonanza |
| DUB ECHO | delay a ½ beat, filtro nel feedback, coda che resta |
| SPACE | FDN a 4 linee (Hadamard), smorzamento |
| CRUSH | quantizzazione 16→3 bit, hold 1→24 frame |
| NOISE | rumore bianco filtrato sommato |
| SWEEP | gate sincronizzato, BPF normalizzato |

- La **profondità** viene da `depth=` (0–200, 100 = centro/bypass, 150 di default), perché la manopola COLOR non è leggibile [C].
- Al centro tutti e sei sono un bypass **bit-exact** [T].
- Un motore per tipo e per deck: cambiando effetto, la coda di DUB ECHO o SPACE continua a suonare sotto il nuovo [T].
- Misure offline [T]:
  - FILTER tutto a sinistra: −78 dB a 5 kHz;
  - prima eco esattamente a ½ beat;
  - coda di SPACE che decade di oltre 70 dB in 2 s;
  - tutti stabili per 10 s a impostazioni massime.

### 3.6 ISOLATOR

- Per banda, il livello va da 0 a 112: 100 = 0 dB, mezzo dB per passo, 0 = kill. Il kill è un bit separato, così il livello si ritrova [C].
- Guadagni smussati [C]; ingresso e uscita dallo split con crossfade di 5 ms [C][T].
- Tutto a 0 dB = nessun calcolo, bypass bit-exact [T].

### 3.7 Interazione con il mixer originale

- Nessuna funzione stock viene disattivata: Color FX nativo, Beat FX, EQ, fader e AUX restano come sono [C].
- COLOR 2, roll di canale e ISOLATOR stanno **prima** del fader: fader, EQ e Color FX nativo agiscono anche su di loro.
- I suoni dell'X-PAD escono sul master **dopo** fader e crossfader, con volume proprio: sono il "terzo canale".
- L'accordo di COLOR 2 consuma **solo** la seconda pressione: il mixer vede il primo pulsante come sempre [C][T].

---

## 4. Controlli ed ergonomia

Riferimento: manuale dell'RMX-1000 (sezione X-PAD FX, [p. 8](https://www.manualsdir.com/manuals/486033/pioneer-rmx-1000-m.html?page=8), [p. 13](https://www.manualsdir.com/manuals/486033/pioneer-rmx-1000-m.html?page=13), [p. 17](https://www.manualsdir.com/manuals/486033/pioneer-rmx-1000-m.html?page=17)).

### 4.1 RMX X-PAD — tab **RMX X-PAD** (dove starebbe STEMS)

Una riga su tutta la larghezza, cinque e cinque come sull'RMX:

```text
[ROLL 1][KICK][SNARE][CLAP][HI HAT][PITCH 0] | [1/8][1/4][1/2][1/1][2/1][VOL 80]
          pulsanti X-PAD FX                  |           striscia
```

- Le due metà hanno lo stesso peso, così il confine tra le finestre dei due deck (x = 640) cade in uno spazio vuoto e nessun controllo viene tagliato. C'è un test di geometria apposito [T].
- **Scegli** ROLL o un suono; un suono suona anche subito.
- **Tocca la striscia e scorri**: ROLL ripete la sorgente, un suono si ripete alla lunghezza della sezione. Al rilascio si ferma, salvo HOLD. Con SHIFT al tocco la striscia è un'ottava più fine (1/16…1/1).
- **PITCH**: tieni e trascina, −12…+12; tocco = 0. **VOL**: tieni e trascina; tocco = mute.
- Colori [C]:
  - rosso: traccia OVERDUB registrata;
  - blu: traccia che suona;
  - arancione su ROLL: OVERDUB in standby.

Pad, con il pannello aperto, su **entrambi** i deck:

| Pad | Premuto | SHIFT + pad |
| --- | --- | --- |
| 1–4 | KICK, SNARE, CLAP, HI HAT | cancella la traccia |
| 5 | ROLL (selezione) | cancella la traccia ROLL |
| 6 | HOLD | PITCH −1 |
| 7 | OVERDUB | PITCH +1 |
| 8 | SOURCE CH1 → CH2 → MST | PITCH 0 |

HOLD + suono = mute della traccia; OVERDUB + suono = cancella, come sull'RMX. HOLD e OVERDUB da soli agiscono al rilascio [C][T].

"Pitch dei vocali": l'ho interpretato come il PITCH dell'RMX, che agisce su suoni e roll. Per cambiare il tono di una traccia vocale sul deck esiste già Key Shift di upstream (±12 per deck) [U].

### 4.2 COLOR 2 — sui pulsanti fisici, senza schermo

- Premi un pulsante Color FX: lo prende il mixer, come stock.
- **Tienilo e premi un secondo pulsante**: il mixer non vede questa pressione e COLOR 2 parte con il secondo effetto, su entrambi i canali.
- Lo stesso accordo, o il secondo pulsante toccato da solo, spegne COLOR 2 [C][T].

Serve prima la sonda (§6) per conoscere i codici dei pulsanti. Proposta per quando la manopola COLOR sarà leggibile (richiede RE): guida l'ultimo effetto toccato; l'altro resta al suo valore con "soft takeover", per evitare salti.

### 4.3 ISOLATOR — tab **ISOLATOR** (posizione STATUS)

```text
[LOW 0] [MID KILL] [HIGH +4]   [FX LOW] [FX MID] [FX HI]      una riga per deck
```

- Banda: tocco = kill / ripristino al livello di prima. Tieni e trascina = livello da kill a +6 dB, con detent a 0 dB.
- FX LOW / MID / HI: le bande su cui agiscono COLOR 2 e i roll di quel deck, accese quando incluse. Tutte o nessuna = segnale intero.
- Ispirazione: i pulsanti LOW / MID / HI dei Beat FX del DJM-900NXS2 ([DJTechTools](https://djtechtools.com/2016/04/04/3-techniques-pioneer-djm-900nxs2s-eq-effects)).

---

## 5. Prestazioni, memoria, latenza

| Voce | Valore | Evidenza |
| --- | --- | --- |
| Latenza aggiunta da roll, COLOR 2, ISOLATOR | 0 campioni | in place, nessun lookahead; il filtro LR4 ha solo il ritardo di fase di un crossover [C] |
| Latenza roll di deck → OVERDUB | dimensione della FIFO al momento, ignota | dipende dal modello di thread [D]; il log riporta underrun e salti |
| Memoria X-PAD | ~18 MB (3 × 3,08 MB roll + 5 × 1,54 MB tracce + 4 × 192 KB suoni), + fino a 6 MB per i WAV | [C] |
| Memoria COLOR 2 | ~1,6 MB | [C] |
| Costo host, ns/frame (Xeon 2,1 GHz, -O2) | FILTER 11, CRUSH 13, NOISE 13, SWEEP 11, SPACE 17, DUB ECHO 23, ROLL 3, X-PAD (5 tracce, ripetizione, roll) 13 | [T]; split a bande non misurato a parte |
| Stima Cortex-A9 | ×10–20 rispetto all'host → caso peggiore ≈ 1–3 µs/frame ≈ **5–13 % di un core** | **stima** [D], non misurata |

Protezioni:
- guardia di sovraccarico: se il blocco master supera metà della sua durata per 16 blocchi di fila, l'X-PAD si bypassa fino al riavvio [C];
- buffer prenotati e azzerati all'avvio, tutto o niente [C][T];
- nessuna allocazione, I/O o attesa nei thread audio e input [C];
- log diagnostico limitato per non saturare il log condiviso (128 KiB) [C].

---

## 6. Build e test offline

Prerequisiti: Python 3.12+, Clang + LLD, `pip install -r requirements.txt`, `hexdump` (per i test della safe mode).

```sh
make hook        # cross-compila librx3_core.so (ARM EABI5) con tutti i moduli
make test        # 467 test: upstream + questo fork
make preflight   # controllo dei file pubblicabili
python3 -m app.preview --out build/pad-preview   # anteprima delle righe touchscreen
```

Test specifici di questo fork:

| Test | Cosa verifica |
| --- | --- |
| `tests/fx/fx_engine_test.c` | Roll: senza click, periodico, ritorno esatto, scorrimento, pitch. Bande: somma piatta, effetto su una banda. X-PAD: spettro dei suoni, ripetizione al frame, OVERDUB. Color FX: bypass, filtri, eco, coda, stabilità. Costi. |
| `tests/test_performance_fx.py` | I moduli attraverso i loro callback con servizi mock. X-PAD: striscia, HOLD, OVERDUB/mute/cancella, PITCH, VOL, pad e LED, geometria della riga. COLOR 2: accordo, consumo delle pressioni, LED, sonda. ISOLATOR: kill, livelli, maschere. Hook `module.sh` in sandbox (anche contro injection). |
| `tests/test_services.py` | Catena audio, servizio bande, filtro LED, argomenti di `SEND_KEY`. |
| `tests/test_runtime_transitions.py` | Gesto della striscia, immagini dei tab dichiarate dalla riga. |

Generare il payload USB (serve la **tua** chiave, fuori dal repository):

```sh
python3 -m app.runtime.cli build --firmware 1.20 \
  --patch rmx-looper --patch dual-color-fx --patch isolator \
  --patch keyshift --patch key-sync --patch beatjump-32bars \
  --key /percorso/della/tua/chiave --output /Volumes/TUA_USB
```

Incompatibilità: `rmx-looper` con `stems`, `isolator` con `samples` (stessi tab). COLOR 2 non ha pannello e non ha conflitti.

Opzioni sulla chiavetta:

| File | Effetto |
| --- | --- |
| `RX3_RUNTIME/dual-color-fx/probe` (vuoto) | attiva la sonda: codici dei tasti, argomenti e ID LED nel log di sessione |
| `RX3_RUNTIME/dual-color-fx/buttons.txt` | riga con sei codici esadecimali (SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER); facoltative `leds=` (sei ID) e `depth=` (0–200) |
| `RX3_RUNTIME/rmx-looper/kick.wav`, `snare.wav`, `clap.wav`, `hat.wav` | sostituiscono i suoni sintetizzati (PCM 16 bit, mono/stereo, 22,05–96 kHz, ≤ 4 s) |

---

## 7. Piano di test su hardware

Da eseguire nell'ordine, a casa, con una chiavetta di prova. Attiva **Troubleshooting report** nell'app. Fermati al primo fallimento e conserva `RX3_RUNTIME/session.txt`.

| # | Prova | Atteso | Cosa guardare nel log |
| --- | --- | --- | --- |
| H1 | Avvio con `rmx-looper`, `dual-color-fx`, `isolator` | interfaccia normale, tab RMX X-PAD e ISOLATOR visibili | `module active:` per i tre, nessun `refused` |
| H2 | Brano su deck 1 per 60 s senza toccare nulla | audio identico allo stock | `rmx deck 1 frames` ≈ 220 500 ogni 5 s a 44,1 kHz; `repeated positions` ≈ 0 |
| H3 | BPM | `rmx deck 1 bpm x100` = BPM mostrato × 100; poi sposta il tempo fader | scopri se segue il tempo fader |
| H4 | ROLL su deck 1: tocca 1/4, scorri fino a 1/1 e torna, rilascia | ripetizione a tempo, nessun click, ritorno al punto giusto del brano | `rmx worst us` molto sotto la durata del blocco |
| H5 | La riga sul touchscreen | nessun controllo tagliato a metà schermo; la striscia segue il dito | — |
| H6 | KICK e CLAP sulla striscia, PITCH ±12, VOL e mute | ripetizioni a tempo, pitch e volume udibili | — |
| H7 | OVERDUB: standby, KICK, roll, HOLD+KICK, OVERDUB+CLAP, OVERDUB | pattern di 4 beat, mute, cancella, stop | `rmx fifo` underrun/skip stabili |
| H8 | ISOLATOR: kill LOW su CH1, poi HIGH +6 | basso via, alti più forti; tutto a 0 = identico allo stock | — |
| H9 | Sonda: file `probe`, premi i 6 pulsanti Color FX in ordine, gira COLOR e AUX | righe `fx2 probe key=…` e `fx2 probe led id=…` | se i pulsanti **non** compaiono, non passano da `SEND_KEY` |
| H10 | `buttons.txt` dalla sonda; tieni FILTER e premi DUB ECHO | FILTER nativo + DUB ECHO software su entrambi i canali; la selezione nativa resta FILTER | — |
| H11 | FX LOW solo + DUB ECHO su COLOR 2 | eco solo sui bassi | — |
| H12 | Stress 60 min: due deck, roll frequenti, OVERDUB, COLOR 2 e ISOLATOR attivi | nessun dropout, nessun crash | nessun `overload guard`; `rmx worst us` stabile |
| H13 | Ritorno allo stock | spegni, togli la chiavetta, riaccendi: console stock | — |

Una funzione può essere considerata per un set solo dopo aver superato H1–H13, e comunque la prima volta con una chiavetta stock di riserva.

---

## 8. Limiti noti

- Nessun test su hardware (§7).
- COLOR 2 non si attiva finché la sonda non ha dato i codici dei pulsanti. Non segue le manopole COLOR/PARAMETER, e i LED dipendono dalla sonda.
- Il VOL dell'X-PAD è sul touchscreen; la manopola AUX non è ancora leggibile.
- I FX LOW/MID/HI agiscono sugli effetti di questo fork, non sui Beat FX nativi.
- Roll e pattern non sono quantizzati alla beatgrid; la striscia segue un solo dito.
- BPM da un accessor non osservato su deck; fallback 120 BPM.
- Conflitti di tab: `rmx-looper` con Stems, `isolator` con Samples. Upstream ha solo tre posizioni di pannello.
- I LED dei pad si aggiornano quando li aggiorna il player.
- Le etichette dei tab sono composte in Liberation Sans, un'approssimazione del carattere del firmware (licenziato, non redistribuibile).
- Il limite delle immagini private è salito da 0x16a5 a 0x16b5. Se reinserisci questa chiavetta in una console che ha già in RAM una versione precedente del mod, viene rifiutata: spegni e riaccendi.

### Ritorno allo stock

Stop → spegni → togli la chiavetta → accendi. Niente viene scritto nella memoria interna. Per saltare il mod a un inserimento, tieni SHIFT mentre inserisci la chiavetta. Per disattivare un solo modulo, deselezionalo nell'app oppure crea con la shell diagnostica uno di questi file:
- `/tmp/rx3-rmx-looper.off`
- `/tmp/rx3-dual-color-fx.off`
- `/tmp/rx3-isolator.off`

---

## 9. Reverse engineering che resta

Prima, senza binario: la **sonda** di COLOR 2 (H9) dice se pulsanti Color FX, manopole COLOR e AUX passano da `SEND_KEY` e con quali argomenti, e registra gli ID dei LED.

Il binario `rbp` non è strippato [U], quindi i nomi C++ sono leggibili. Con il binario in mano, nell'ordine:

1. `python3 scripts/check_addresses.py rbp`: conferma che tutti gli indirizzi e i guard di questo fork, inclusi `RMX_PLAY_BPM` e `FX2_PLAY_BPM`, corrispondono al tuo firmware.
2. `python3 scripts/find_fx_symbols.py rbp > fx-symbols.txt`: elenca le classi Color FX, mixer, LED, beatgrid e beat jump.
3. Dalle classi Color FX:
   - ctor/initialize/execute e `adjustParameter` (come per Pitch) → seconda istanza **nativa**;
   - campo del valore COLOR/PARAMETER → manopole fisiche per COLOR 2;
   - lettura della manopola AUX → VOL dell'X-PAD.
4. Instradamento dei Beat FX → bande LOW/MID/HI anche per i Beat FX nativi.
5. Fase della beatgrid del deck → roll e pattern quantizzati.
6. Tabella del Beat Jump → ±16 con lo stesso metodo di ±32.

Ottenere `rbp` richiede la chiave del produttore e il pacchetto firmware, previa accettazione dei termini. Upstream documenta:
- la procedura, in `REFERENCES.md` (*Getting a root filesystem from the published GPL sources*);
- la sua posizione legale, in `LEGAL.md` (Direttiva 2009/24/CE artt. 5(3) e 6).

Non l'ho fatto perché è una decisione tua; nessun file del produttore va mai committato.

---

## 10. RMX-1000: cosa è stato preso, e cosa no

Fonti:
- il manuale dell'RMX-1000 ([manualsdir](https://www.manualsdir.com/manuals/486033/pioneer-rmx-1000-m.html?page=8));
- la [pagina prodotto Pioneer DJ](https://www.pioneerdj.com/en/product/dj-effectors/rmx-1000/);
- [DJWORX](https://djworx.com/review-pioneer-rmx-1000-remix-station/).

Preso dall'RMX-1000:
- **X-PAD FX**: ROLL e quattro suoni (KICK, SNARE, CLAP, HI HAT) scelti con pulsanti, suonati sulla striscia X-PAD con lunghezze ritmiche, e **HOLD**.
- **OVERDUB**: registra i colpi dei pad in un pattern che gira. HOLD + suono = mute, OVERDUB + suono = cancella, OVERDUB di nuovo = stop.
- **PITCH**: agisce sui suoni (qui anche sui roll) e **VOLUME** dei suoni.

Non replicati: SCENE FX, ISOLATE FX dell'RMX (qui sostituito da ISOLATOR sul deck), i campioni originali Pioneer (i suoni sono sintetizzati, oppure tuoi WAV) e l'uscita dedicata.
