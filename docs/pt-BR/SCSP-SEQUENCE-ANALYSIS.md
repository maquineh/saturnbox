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

## Limite desta validação

Dizer "este é o tiro" ou "este é o WARNING" exige correlacionar uma entrada com
a ação do jogo. Isso não foi feito aqui: a imagem do disco
(`H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf`, caminho padrão do launcher) não
estava acessível nesta sessão (`H:\Isos` não existe na máquina). Sem rodar a
fase com o debugger, o máximo que os dados provam é o caráter acústico e a
mecânica de cada entrada. Para fechar a identificação auditiva é preciso:

1. montar a imagem do disco e rodar `launch_x4_debug.ps1 -ScspDriver`;
2. capturar o áudio em combate com os contadores `slot_mask` e os pacotes `01`
   de cada evento (o debugger já publica as contagens de comando);
3. comparar o `bank:seq` do pacote com o catálogo acima e confirmar com o
   jogador que o som corresponde ao tiro, ao dash, ao WARNING ou à explosão.

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
```
