# Sequências do X4 executadas pelo driver real

Este documento fecha a pergunta "o que cada sequência do banco sonoro do X4
faz?" executando o driver 68000 de verdade, não o modelo HLE. O insumo é o
snapshot `scsp_ram.bin` da captura de fase
(`build-saturnbox/rockman/stage-from-title-capture-20261027/video-diagnostics/`),
que contém `SDDRVS.TSK` no offset 0 e os cinco bancos de sequência já
carregados. Nada do jogo é distribuído no repositório.

## Como uma sequência é acionada

O `saturn-scsp-probe` ganhou três opções para isso:

- `--play-sequence BANK:SEQ` enfileira o pacote `01 00 CC BB SS` no mailbox de
  comandos e espera o driver consumi-lo.
- `--sequence-control N` escolhe o byte de controle (`CC`) e `--sequence-slot N`
  escolhe qual dos oito pacotes de 16 bytes usar.
- `--trace-reads START:END PATH` registra cada leitura da RAM de som nesse
  intervalo, com PC e ciclo; `--dsp-window-log PATH` faz o mesmo para escritas
  em `0x100700`–`0x100EFF`.

Dois detalhes do mailbox foram descobertos nesta rodada e são necessários para
qualquer replay:

1. **O pacote 0 não é um comando.** A fila em `0x700` tem um cabeçalho nos
   primeiros 16 bytes; o driver varre os pacotes a partir de `0x710`. Injetar
   `01 ...` em `0x700` não faz nada.
2. **O comando `08` (carregar mapa) precisa ter rodado antes.** Sem ele o
   driver não publica a tabela em `0x500` e um `01` é ignorado. Por isso o
   caminho usado é `--replay-pending --play-sequence`, nessa ordem.

O byte de controle importa: com `CC=0` o driver consome o pacote e não inicia
nada; com `CC=3` ou `CC=6` a sequência toca. Isso explica por que a varredura
testa uma lista de controles.

## Estrutura do contêiner

O player lê a entrada assim (offsets relativos ao início da entrada):

| offset | tamanho | conteúdo | evidência |
| --- | --- | --- | --- |
| `0x00` | u16 | primeiro cabeçalho (`0x0030` em 113/113) | leitura em PC `0x4EDC` |
| `0x02` | u16 | **nunca lido** (`0x0001` em 113/113) | ausência no trace |
| `0x04` | u16 | cabeçalho | PC `0x5B38` |
| `0x06` | u16 | cabeçalho | PC `0x5B46` |
| `0x08` | u32 | cabeçalho longo | PC `0x5B84` |
| `0x0C` | u32 | último campo de cabeçalho | PC `0x5B86` |
| `0x10` | … | fluxo de eventos | despacho em `0x57B8` |

O fluxo de eventos é uma sequência de opcodes de um byte; alguns opcodes têm
argumentos. O trace da entrada **banco 3, sequência 2** (`0x29AD4`, 42 bytes)
mostra 13 bytes despachados em `0x57B8` e 26 bytes lidos por outros endereços
(14 de cabeçalho e 12 argumentos), com um byte (`0x29AFC`) não lido dentro da
janela observada. Opcodes `0x88`–`0x8B` não leram argumento; `0xB0` e `0xC0`
leram dois argumentos (PCs `0x5A94`/`0x5A96` e `0x5A7E`/`0x5AAC`). O byte
variável da família (`0x24`) é lido por um handler diferente, em `0x57C4`.

Nada disso prova semântica de opcode: o trace mostra **quem** leu cada byte, não
o que o handler fez com ele. O que ele faz está abaixo, medido pelas escritas de
registrador.

## O que o player faz com uma sequência

Trace de escritas de registrador para a mesma entrada, depois do comando:

```
cyc=265220 pc=0x3878 addr=0x10000b val=0x1f   ; RR = 31 (release rápido)
cyc=265220 pc=0x38c8 addr=0x100000 val=0x10   ; PCM8B
cyc=265478 pc=0x38f8 addr=0x100000 w=4 0x15baf2 ; SA = 0x5BAF2, SSCTL=0
cyc=265478 pc=0x38fc addr=0x10000e val=0x0    ; MDL/MDXSL/MDYSL = 0
cyc=265478 pc=0x3902 addr=0x100004 w=4 0x15ff ; LSA=0, LEA=0x15FF
cyc=265478 pc=0x3908 addr=0x100008 w=4 0x1f001f ; AR=31, RR=31
cyc=267010 pc=0x3894 addr=0x100000 val=0x18   ; KYONB | KEYONEX
```

Ou seja: o driver é um **sequenciador de registradores**. Ele escolhe a amostra
(`SA`), os limites de loop, o envelope e a afinação, e liga o slot. Não há lista
de notas nem instrumentos: o evento é um comando de baixo nível sobre o chip.
Também confirma os contadores da varredura: `SSCTL=0` (PCM), `LFO=0`,
`MOD=0`, `ISEL/IMXL=0` (sem envio ao DSP).

## Varredura completa: 113 entradas

`tools/diagnostics/scan_x4_sequences.py` roda o probe uma vez por entrada (para
não misturar estados), tenta os controles 3, 6 e 0, e grava
`sequence-scan.csv` mais um WAV por entrada audível.

| métrica | valor |
| --- | --- |
| entradas examinadas | 113 (bancos 0–4: 30, 13, 21, 25, 24) |
| com áudio | 69 |
| silenciosas | 44 |
| com `noise` | **0** |
| com LFO de pitch ou amplitude | **0** |
| com modulação (FM) | **0** |
| com passos de DSP | **0** |

O mesmo resultado apareceu com controle 3 e com controle 6, então não é efeito
do canal escolhido. Combinado com a
[análise estática](SCSP-DRIVER-STATIC-ANALYSIS.md), que mostra que o driver tem
um caminho para carregar programa de DSP mas nunca recebe a imagem
(`[0x8858]=0`, janela `0x7000–0x8838` zerada), a conclusão é: **nessas capturas
o X4 não usa noise, LFO, FM nem o DSP**. Os efeitos e vozes que faltavam vêm de
slots PCM diretos.

## Catálogo de áudio

`tools/diagnostics/catalog_x4_sequence_audio.py` mede cada WAV e classifica o
caráter. Os 69 audíveis se dividem em 46 sustentados, 11 curtos e brilhantes,
10 misturas polifônicas e 2 curtos tonais. Os curtos e brilhantes são os
candidatos naturais a efeito pontual:

| banco | seq | controle | duração ativa | pico | centroide | fundamental |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 11 | 3 | 0,083 s | 0,72 | 2342 Hz | 495 Hz |
| 2 | 3 | 3 | 0,086 s | 0,64 | 1436 Hz | 258 Hz |
| 1 | 3 | 3 | 0,118 s | 0,36 | 2276 Hz | 474 Hz |
| 0 | 13 | 3 | 0,129 s | 0,72 | 2271 Hz | 1206 Hz |
| 2 | 2 | 3 | 0,136 s | 0,65 | 2168 Hz | 431 Hz |
| 0 | 17 | 3 | 0,164 s | 0,72 | 2624 Hz | 991 Hz |
| 2 | 5 | 3 | 0,323 s | 0,73 | 3216 Hz | 904 Hz |

Os WAVs ficam em `build-scsp/tmp-test/sequence-scan/wav/` e o CSV completo em
`build-scsp/tmp-test/sequence-catalog/sequence-audio-catalog.csv`. O comando
que o jogo emitiu durante o combate capturado (`01 00 03 02 0B`, controle 3,
banco 2, sequência 11) corresponde a um som **sustentado** de ~0,4 s, o que é
compatível com uma trilha curta ou um efeito com cauda, não com um disparo
seco.

### Taxa de gatilho por entrada (rota longa)

Uma segunda execução rodou 600 milhões de blocos (714 s de áudio, 4.020
key-ons, 3.541 pedidos, 34 tipos de sequência) com direita mantida, disparo a
cada 3M de blocos, salto a cada 7M e dash a cada 17M. Para cada tipo de
sequência, a tabela mostra a fração dos eventos de entrada seguidos por um
pedido daquele tipo em até 50 mil blocos (~60 ms):

| sequência | pedidos | após B | após A | após C | só direita |
| --- | --- | --- | --- | --- | --- |
| `c2 b1 s0` | 112 | **0,68** | 0,15 | 0,07 | 0,00 |
| `c3 b2 s6` | 50 | **0,31** | 0,04 | 0,04 | 0,00 |
| `c3 b2 s2` | 31 | **0,19** | 0,01 | 0,00 | 0,00 |
| `c3 b2 s3` | 26 | **0,16** | 0,08 | 0,04 | 0,00 |
| `c0 b0 s19` | 136 | 0,07 | **0,60** | 0,11 | 0,07 |
| `c1 b1 s7` | 39 | 0,06 | **0,51** | 0,07 | 0,00 |
| `c7 b1 s11` | 48 | 0,03 | **0,24** | 0,04 | 0,00 |
| `c2 b1 s4` | 9 | 0,01 | 0,00 | **0,14** | 0,00 |
| `c5 b3 s5` | 52 | 0,00 | 0,00 | 0,00 | 0,01 |
| `c5 b3 s7` | 26 | 0,01 | 0,01 | 0,04 | 0,00 |
| `c5 b3 s12` | 24 | 0,00 | 0,00 | 0,00 | 0,00 |
| `c5 b3 s13` | 24 | 0,01 | 0,00 | 0,00 | 0,00 |

Leitura: o disparo não é um som só — ele pede `c2 b1 s0` em 68% dos casos e
`c3 b2 s6/s2/s3` com menor frequência (provavelmente camadas: cano, tiro e
acerto). O salto pede `c0 b0 s19` e `c1 b1 s7`; o dash pede `c2 b1 s4`. Já
`c5 b3 s5/s7/s12/s13` (a família one-shot de 42 bytes) **não** correlaciona com
nenhuma entrada: são eventos do jogo (acerto/explosão/dano). Essa é a evidência
mais próxima de uma explosão que os dados dão, mas ainda falta vê-la acontecer.

### A rota longa não chega ao chefe

Os 34 tipos de sequência aparecem todos até ~180 s e continuam até o fim: não
surge nenhum tipo novo na segunda metade, o que seria esperado se a música ou o
aviso do chefe entrassem. Os tipos pedidos aos 21 s reaparecem aos ~701 s,
padrão compatível com reinício de fase depois de o jogador morrer. Com entrada
cega (sem desviar de inimigos e abismos) não foi possível atravessar a fase:
**WARNING não foi exercitado**.

Para fechar isso, o caminho certo é uma sessão jogada por uma pessoa com o
debugger. `launch_x4_stage.ps1 -ScspDriver` agora grava
`--audio-output` e `--scsp-requests` automaticamente, então basta jogar até o
chefe e me passar o diretório da sessão: o mesmo cruzamento gatilho→pedido
identifica o WARNING e as explosões.

### Ferramentas de depuração para chegar ao chefe

Para tentar atravessar a fase sem um jogador humano, o `saturn-sh2` ganhou duas
opções de depuração e há um script de apoio:

- `--poke STEP:ADDR:VALUE[:WIDTH]` (repetível, passos crescentes) escreve na
  memória do convidado uma vez por passo. Serve para manter o jogador vivo ou
  reposicioná-lo em rotas automáticas; não é usado em execução normal.
- `--watch-memory ADDR:LENGTH:PERIOD` com `--watch-dir DIR` grava uma cópia
  binária da região a cada PERIOD passos (amostragem a partir de
  `--watch-from`).
- `tools/diagnostics/find_x4_health.py` compara essas cópias e ranqueia offsets
  com cara de contador pequeno que cai e volta.

Resultados dessas tentativas, todos negativos e registrados para não serem
repetidos:

| tentativa | resultado |
| --- | --- |
| rota cega 205M (tiro + direita) | reinicia a fase, sem chefe |
| rota cega 600M (tiro + salto + dash) | mesmo conjunto de 34 cues até o fim, sem cue novo |
| 122 amostras de WRAM (0x06050000, 64 KiB, 1M de passos) | 2.815 candidatos, nenhum com forma de vida (a maioria é tabela de objetos/RNG) |
| warp de posição (`--poke` em 0x060545A8, +1 unidade a cada 2M de passos) | mesmo conjunto de 33 cues; a posição é reescrita pela física do jogo |
| renders longos (20M de ciclos) de bancos 0 e 1 | nenhum motivo de sirene inequívoco; durações de 0,58 a 1,75 s |

Conclusão operacional: **chegar ao chefe exige jogo humano** (ou um savestate na
porta do chefe). O launcher já grava áudio e todos os comandos, então a sessão
jogada é o único insumo que falta.

### Chegada ao chefe com pokes de vida (29/10)

A varredura de snapshots apontou a vida do jogador nos bytes
`0x060545FC`–`0x060545FE`: os três valem `0x20` (32) com a barra cheia, caem
juntos a cada dano e voltam a `0x20` no respawn (a série completa é
`32,29,26,20,18,15,13,13,8,…,3,0,32,32,…`). Com `--poke-file` reescrevendo
esses bytes a cada 150 mil passos, a rota de 400M de blocos (disparo a cada 3M,
salto a cada 7M) sobreviveu até o fim, em vez de reiniciar a fase.

O resultado aparece na linha do tempo dos pedidos:

| tempo | evento |
| --- | --- |
| até 350,7 s | `c6b3s2`/`c7b3s2` (base/ambiência) retriggerados a cada ~0,6 s |
| 350,7–352,3 s | **nenhum pedido** por ~1,5 s: a base para |
| 352,3 / 352,9 / 353,2 s | `c5b3s7`, `c5b3s5`, `c5b3s5` — três one-shots do banco 3 |
| 355,2 s → 404,4 s | `c4b0s2` (70 pedidos) e, de 362,3 s, `c5b0s3` (79) |
| 356,7 e 360,1 s | `c6b3s14` (one-shot do banco 3) |
| 365,2 / 366,7 s | `c1b0s15` e `c1b0s16` |
| 404,0 s | `c7b1s12`, e a seção nova termina |

O áudio acompanha: o RMS salta de ~0,15 para ~0,25 e o pico satura em 1,0 a
partir de ~356 s, contra pico ~0,7 antes. Três coisas ficam evidentes:

1. a rota passou a alcançar uma seção nova por volta de 350–356 s, coerente com
   a chegada do chefe;
2. os cues que só aparecem nessa seção delimitam o WARNING (ver abaixo);
3. a seção nova satura a saída em 1,0, o que é um problema de ganho separado.

### O que é exclusivo da seção do chefe

Comparando este log com os das rotas anteriores (205M, 600M e warp), seis tipos
aparecem **só** na seção nova:

| cue | pedidos | primeira vez | leitura |
| --- | --- | --- | --- |
| `c4 b0 s2` | 70 | 355,2 s | base/música da seção (repete) |
| `c5 b0 s3` | 79 | 362,3 s | base/música da seção (repete) |
| `c6 b3 s14` | 2 | 356,7 s | one-shot do banco 3, exclusivo da transição |
| `c1 b0 s15` | 2 | 365,2 s | sequência longa do banco 0 |
| `c1 b0 s16` | 2 | 365,7 s | sequência longa do banco 0 |
| `c7 b1 s12` | 1 | 404,0 s | fecha a seção |

Já os one-shots `c5 b3 s5`/`c5 b3 s7`, tocados nas 352–353 s, **não** são
exclusivos: na rota de 600M eles foram pedidos 52 e 26 vezes durante o combate.
São sons de acerto/dano, não um aviso único. Por exclusividade, o WARNING é
`c6 b3 s14` (voz/one-shot, duas vezes em 3,4 s logo depois de a base parar) ou
`c1 b0 s15`/`c1 b0 s16` (sequências longas, duas vezes cada).

### Casamento por forma de onda: negativo e explicado

Tentei casar o áudio da fase com os WAVs gerados pelo probe. A correlação
máxima foi 0,44 (`b3 s7` na janela 351–355 s) e 0,56 (`b3 s2` na janela
167–172 s), longe do ~1,0 esperado para o mesmo som isolado. A hipótese de que
`05` e `14` continham volume/pan/pitch por canal estava errada. No guia Sega,
`05` define volume e fade da sequência, `0E` controla o pan da sequência, `14`
escolhe o modo de tempo e `15` aplica a razão relativa de tempo. Esses comandos,
o mapa PCM ativo e o estado dos registradores ainda podem explicar a diferença,
mas a correlação não identifica qual fator faltou sem os pacotes completos do
mesmo evento. A captura futura precisa guardar os 16 bytes de cada comando e o
estado SCSP sincronizado; consulte o
[guia Sega de comandos de som](https://www.infochunk.com/saturn/segahtml_en/sndt/prog/drve/sndcmd_.htm).

As janelas de áudio estão em `build-scsp/tmp-test/boss-clips/`
(`09-transition-350-356.wav`, `08-three-oneshots-352.2-354.6.wav`,
`03-c6b3s14-355-360.wav`, `04-c1b0s15-16-364-369.wav`,
`05-boss-section-370-378.wav`). Nomear o WARNING exige escuta: a mecânica, o
instante e o conjunto exclusivo estão medidos; o nome depende de ouvir os
clipes.

### Caráter espectral dos one-shots do banco 3

Medindo os renders do probe (energia acima de 4 kHz, planura espectral e
cruzamentos de zero) os três candidatos se separam:

| cue | pico | energia >4 kHz | planura | cruzamentos/s | leitura |
| --- | --- | --- | --- | --- | --- |
| `b3 s14` | 0,60 | 5,8% | 0,059 | 1039 | tonal, grave — compatível com voz/jingle |
| `b3 s5` | 0,71 | 10,6% | 0,111 | 1906 | misto |
| `b3 s7` | 0,58 | 16,1% | 0,163 | 3056 | ruidoso e brilhante — compatível com impacto/explosão |

Isso é coerente com a leitura por gatilho: `b3 s7`/`b3 s5` são os sons de
acerto (evento, sem correlação com entrada) e `b3 s14` é o mais tonal dos três,
aparecendo só na transição do chefe. Ainda assim, "tonal" não distingue uma voz
de um jingle: essa parte continua dependendo de escuta.

### Evidência visual: o chefe, a explosão e a ausência de um cue de WARNING

Capturas determinísticas de quadro com `--frame-capture-after-blocks` no mesmo
percurso (sete instantes, todos em `build-scsp/tmp-test/frame-*/`):

| bloco | tempo | o quadro mostra |
| --- | --- | --- |
| 278.000.000 | 331,0 s | cidade, placa "CRTCOM", **sem** chefe e sem barra de chefe |
| 280.000.000 | 333,3 s | máquina grande à direita e barra de vida de chefe já presente |
| 282.000.000 | 335,7 s | Vile (armadura verde) com a barra de chefe e clarão amarelo atrás |
| 286.000.000 | 340,5 s | barra de chefe, chefe fora de quadro |
| 290.000.000 | 345,2 s | Vile em pé, barra de chefe, X à direita |
| 294.000.000 | 350,0 s | barra de chefe e clarão amarelo grande |
| 296.000.000 | 352,4 s | o chefe não está mais na cena |

Nenhum dos sete quadros mostra o texto "WARNING". A chegada do chefe fica
entre 331,0 s (sem barra) e 333,3 s (com barra), portanto o banner, se existe,
está nesse intervalo de ~2 s.

Com isso, o desfecho do chefe fica localizado: ele aparece entre 331 e 333 s, a
luta segue até ~350 s, há explosões e em 352,4 s o sprite já não está. Os três
one-shots ruidosos de 352,3–353,2 s (`b3 s7`, `b3 s5`, `b3 s5`) coincidem com
esse desfecho, então a **explosão fica identificada por três evidências
independentes**: visual (clarão e chefe ausente), espectral (o som mais ruidoso
do banco 3) e de gatilho (evento, sem correlação com entrada).

Já o aviso do chefe **não tem sequência dedicada** nesta captura: entre 320 e
355 s os únicos pedidos são os habituais da fase (`b3 s2`, `b3 s7`, `b3 s5`,
`b0 s19`, `b2 s0/s3/s6`, `b1 s0/s1/s3`), e o primeiro cue novo é `c4 b0 s2` em
355,25 s, depois da luta. A música da fase continua tocando na chegada do
chefe. Como o próprio player de sequência é quem aciona as amostras dentro da
música já em execução, um eventual aviso sonoro viria dessa sequência — que o
emulador reproduz integralmente. Medindo onsets no áudio entre 326 e 342 s, os
picos (764 em 338,0 s, 707 em 335,3 s) são só ~2× o p95 da janela anterior
(377), sem um evento isolado que se destaque: não há som novo faltando ali.

## A família de 42 bytes é um one-shot parametrizado

Sessenta e nove das 113 entradas têm exatamente 42 bytes (banco 3: 25, banco 4:
24, banco 2: 20). Elas são o mesmo template:

- todas começam com `00 30 00 01 00 10 00 08`, que são os campos de cabeçalho;
- dentro de um mesmo banco, o **único** byte que muda é o `0x24`, que assume
  exatamente o índice da entrada (0..24 no banco 3, 0..23 no banco 4, 0..20 no
  banco 2);
- entre bancos, além do `0x24`, mudam `0x0D`–`0x0F` (parte do u32 de `0x0C`) e
  o `0x12` (1 a 4), que ficam constantes dentro de um banco;
- o trace de leitura mostra o `0x24` sendo lido por um handler em PC `0x57C4`,
  diferente do despachante em `0x57B8` — é um parâmetro, não um opcode.

O áudio confirma a leitura: as 25 entradas do banco 3 renderizam ~0,50 s cada,
com o mesmo formato de envelope e picos entre 0,36 e 0,73, mudando a altura e o
centroide (de 471 Hz a 3238 Hz). Ou seja, o template toca **uma amostra por
entrada** com envelope fixo: são one-shots parametrizados, e o `0x24` escolhe
qual amostra do banco PCM tocar.

Na fase, os pedidos `c5 b3 s5`, `s7`, `s12` e `s13` aparecem entre 0,5 s e 1,3 s
depois de um disparo, o que é compatível com sons de impacto/acerto
disparados quando o tiro encontra algo. A identificação nominal de cada um
desses 25 one-shots ainda depende de vê-los na fase com inimigos.

## Validação na fase (28/10/2026)

A imagem do disco foi disponibilizada em
`C:\Users\p001209\Downloads\rockmanx4\Rockman X4 (J)(Saturn)(ISO).mdf` e a rota
de fase rodou com o driver real e captura de áudio:

```powershell
saturn-sh2 boot build\rockman\aip.bin --aip-base 0x06002E40 `
  --ip build\rockman\ip.bin --ip-base 0x06002000 `
  --program build\rockman\0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc 'C:\Users\p001209\Downloads\rockmanx4\Rockman X4 (J)(Saturn)(ISO).mdf' `
  --disc-sector-size 2448 --continue-after-execdmp `
  --vblank-blocks 14000 --steps 205000000 --pad-sequence $pad `
  --renderer cpu --scsp-driver `
  --audio-output build-scsp\tmp-test\stage-gameplay.wav `
  --scsp-requests build-scsp\tmp-test\stage-requests.csv
```

Resultado da execução (`status: step-limit`, 205.000.000 de blocos):

| métrica | valor |
| --- | --- |
| frames de áudio | 14.642 |
| amostras | 10.761.870 (244,03 s) |
| ciclos do 68000 | 2.754.142.326 |
| key-ons / key-offs | 473 / 336 |
| escritas em registradores | 419.849 |
| `noise` / `lfo` / `fm` / `dsp-steps` | 0 / 0 / 0 / 0 |
| pedidos de sequência (`01`) | 424 |

O `saturn-sh2` passou a gravar cada pacote de comando com o passo do scheduler
(`--scsp-requests`), não só o `01`: numa execução curta de 60M de blocos
aparecem `08` (carregar mapa) 16×, `82` (volume) 5×, `85`/`86`/`87`/`8A` (PCM)
9/10/4/3×, além de 28 `01`. O arquivo permite alinhar o pedido ao quadro de
áudio: com 14.000 passos por quadro e 735 amostras, o tempo é `passo / 840.000`
s. Os scripts de análise filtram `command = 0x01`, que é o que inicia sequência.

### Efeitos identificados pelo gatilho

Os pressionamentos roteirizados foram comparados com o pedido mais próximo
(`tools/diagnostics/probe_x4_gameplay_onsets.py`, que também grava uma janela
WAV por pressionamento):

| botão | passo | tempo | pedido mais próximo | atraso |
| --- | --- | --- | --- | --- |
| B | 160.000.000 | 190,476 s | `c3 b2 s3` | +25,0 ms |
| B | 168.000.000 | 200,000 s | `c3 b2 s2` | +18,9 ms |
| B | 180.000.000 | 214,286 s | `c3 b2 s2` | +21,3 ms |
| B | 184.000.000 | 219,048 s | `c3 b2 s3` | +20,9 ms |
| B | 188.000.000 | 223,810 s | `c3 b2 s6` | +14,0 ms |
| B | 192.000.000 | 228,571 s | `c3 b2 s2` | +12,4 ms |
| B | 196.000.000 | 233,333 s | `c3 b2 s3` | +24,7 ms |
| C | 174.000.000 | 207,143 s | `c2 b1 s4` | +35,8 ms |
| A | 178.000.000 | 211,905 s | `c0 b0 s19` | +18,7 ms |
| A | 186.000.000 | 221,429 s | `c0 b0 s19` | +19,2 ms |

O padrão é inequívoco: **B (disparo) pede as sequências `b2 s2`, `b2 s3` ou
`b2 s6` no controle 3**, com 12–25 ms de atraso, o que é menos de dois quadros
(e portanto coerente com o polling do pad pela SH-2). **C (dash) pede `b1 s4`
no controle 2** e **A (salto) pede `b0 s19` no controle 0**, os dois com ~19–36
ms. Onde o pressionamento caiu junto de outro evento, o pedido mais próximo foi
outro (`b1 s0`, `b0 s1`, `b3 s2`, `b0 s10`), como esperado em combate.

Cada pressionamento tem uma janela WAV em
`build-scsp/tmp-test/gameplay-onsets/windows/`, com o pico e o fluxo espectral
medidos no CSV ao lado, para escuta direta.

### O que este teste prova e o que não prova

- Prova que, na fase, o driver real está tocando: 473 key-ons, 2,75 bilhões de
  ciclos do 68000 e 244 s de áudio não silencioso, com `noise`, `lfo`, `fm` e
  `dsp` em zero também aqui.
- Prova que o disparo, o dash e o salto pedem sequências distintas e
  determinísticas, com latência de 12–36 ms. Isso identifica **quais** entradas
  são o tiro, o dash e o salto; não depende de escuta.
- **Não** identifica WARNING nem explosão: a rota cobre ~80 s de fase e não
  chega ao chefe nem garante destruir inimigos. Faltam esses dois eventos.
- A tentativa de casar a forma de onda da janela capturada com o WAV offline
  (`match_x4_cues.py`) deu correlação baixa: a captura satura em 1,0 em vários
  trechos (mistura de música e efeitos) e o casamento por perfil espectral
  (`analyze_x4_gameplay_audio.py`) não separa uma voz da cama musical. Os dois
  scripts ficam como registro do método e da limitação.

### Próximo passo para WARNING e explosão

Estender a rota até o chefe (mais blocos, com dash e salto no caminho) e
capturar o intervalo do WARNING e das explosões de inimigos, repetindo a
correlação input→pedido que funcionou para o tiro. Alternativa mais curta:
usar um estado salvo na porta do chefe.

## Limites restantes

O que ainda falta, depois das duas rodadas na fase:

- **WARNING e explosão**. As duas rotas cegas (205M e 600M de blocos) cobrem a
  primeira fase e reiniciam sem chegar ao chefe. Os candidatos a
  acerto/explosão existem (`c5 b3 s5/s7/s12/s13`, sem correlação com entrada),
  mas nomeá-los exige ver o evento acontecer. O caminho é uma sessão jogada por
  uma pessoa com `launch_x4_stage.ps1 -ScspDriver`, que já grava áudio e
  pedidos.
- A identificação de tiro, salto e dash vem do gatilho (entrada → pedido), que
  é forte e reprodutível, mas não é escuta humana: as janelas WAV por
  pressionamento permitem essa confirmação.
- O casamento por forma de onda entre a captura e o WAV offline não fecha, e a
  hipótese principal é que a captura de RAM usada no offline tenha uma versão
  diferente dos bancos PCM em relação ao estado do jogo aos ~190 s. Uma captura
  de RAM no mesmo instante resolveria a dúvida.

## Reprodução

```powershell
# 1. bancos e tabelas a partir do dump
python tools\diagnostics\analyze_x4_scsp_driver.py `
  build-saturnbox\rockman\stage-from-title-capture-20261027\video-diagnostics\scsp_ram.bin `
  --out-dir build-scsp\tmp-test\sequence-analysis

# 2. varredura das 113 entradas com áudio
python tools\diagnostics\scan_x4_sequences.py `
  --ram build-saturnbox\rockman\stage-from-title-capture-20261027\video-diagnostics\scsp_ram.bin `
  --probe build-scsp\tools\saturn-scsp-probe\Release\saturn-scsp-probe.exe `
  --out build-scsp\tmp-test\sequence-scan --cycles 6000000 --controls 3,6,0 --wav

# 3. estrutura e anotação de trace
python tools\diagnostics\decode_x4_sequences.py `
  --ram build-scsp\tmp-test\sequence-scan\map-ram.bin `
  --scan build-scsp\tmp-test\sequence-scan\sequence-scan.csv `
  --out build-scsp\tmp-test\sequence-decode

# 4. catálogo de áudio (precisa de numpy)
python tools\diagnostics\catalog_x4_sequence_audio.py `
  --scan build-scsp\tmp-test\sequence-scan\sequence-scan.csv `
  --wav-dir build-scsp\tmp-test\sequence-scan\wav `
  --out build-scsp\tmp-test\sequence-catalog

# 5. janelas e onset de cada pressionamento na fase (precisa de numpy)
python tools\diagnostics\probe_x4_gameplay_onsets.py `
  --audio build-scsp\tmp-test\stage-gameplay.wav `
  --requests build-scsp\tmp-test\stage-requests.csv `
  --presses 160000000:b,174000000:c,178000000:a `
  --out build-scsp\tmp-test\gameplay-onsets
```
