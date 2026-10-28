# Análise estática do driver SCSP do Rockman X4

Este documento registra a leitura estática de dois dumps de RAM de som
(`scsp_ram.bin`, 524.288 bytes cada) capturados de sessões reais do X4, com o
driver `SDDRVS.TSK` carregado a partir do offset 0. Nada aqui executa o 68000:
todas as conclusões vêm de bytes do dump e de instruções decodificadas à mão,
com os offsets e as palavras de opcode citados para conferência. A ferramenta
usada é `tools/diagnostics/analyze_x4_scsp_driver.py`, que produz as seções
1 a 5; a seção 4.5 foi verificada na mão a partir da listagem que a ferramenta
imprime em 4.4.

Os dois dumps são:

| dump | sha256 |
| --- | --- |
| `build-saturnbox/rockman/title-capture-single/video-diagnostics/scsp_ram.bin` | `3b2044939a2ceb6b150f9335dfcc059898457b42c222e7da771c70ebc1b87641` |
| `build-saturnbox/rockman/stage-from-title-capture-20261027/video-diagnostics/scsp_ram.bin` | `15cebdbaaa9d42fc0e5fcd454d9500dcd01302256425469b70ee2ee247bd8967` |

Nos dois, os vetores de reset são SP=`0x0000A000` e PC=`0x00001000`. As 113
entradas de sequência recuperadas são **idênticas** byte a byte entre os dois
dumps, e as cinco tabelas de banco também.

## Como reproduzir

```powershell
python tools\diagnostics\analyze_x4_scsp_driver.py `
  build-saturnbox\rockman\title-capture-single\video-diagnostics\scsp_ram.bin `
  build-saturnbox\rockman\stage-from-title-capture-20261027\video-diagnostics\scsp_ram.bin `
  --out-dir build-scsp\tmp-test\sequence-analysis
```

O script aceita um ou mais dumps, só usa a biblioteca padrão do Python e grava
`scsp-driver-analysis.json` e `scsp-driver-entries.csv` em `--out-dir`.
Opções: `--max-differences` (padrão 6) limita o número de offsets que ainda
caracteriza uma família; `--minimum-steps` (padrão 4) é o mínimo de passos para
um candidato a programa de DSP; `--skip-whole-ram-dsp-scan` restringe a
varredura de DSP à imagem do driver.

Artefatos desta execução em `build-scsp/tmp-test/sequence-analysis/`:
`report-both.txt` (saída completa), `scsp-driver-analysis.json` (todos os
registros) e `scsp-driver-entries.csv` (as 113 entradas em hex).

## 1. Tabelas de sequência recuperadas

A varredura por tabelas de banco (u16 big-endian de contagem seguido de
`count` offsets u32 big-endian, estritamente crescentes, começando depois do
fim da tabela e dentro de um comprimento plausível) encontrou **63
candidatos** nos endereços pares do dump. Cinco têm o primeiro offset
exatamente no fim da tabela (grau A, o formato que o driver usa); os outros 58
são tabelas de uma entrada cujo único offset cai longe do fim (grau C) —
ruído de código/dados, listado no relatório e no JSON, não aceito como banco.

| banco | tabela | entradas | fim da tabela | primeira entrada | última entrada | extensão |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | `0x28D60` | 30 | `0x28DDA` | `0x28DDA` | `0x293C0` | `0x28D60`–`0x2945C` |
| 1 | `0x2945C` | 13 | `0x29492` | `0x29492` | `0x29632` | `0x2945C`–`0x29654` |
| 2 | `0x29654` | 21 | `0x296AA` | `0x296AA` | `0x299F0` | `0x29654`–`0x29A1A` |
| 3 | `0x29A1A` | 25 | `0x29A80` | `0x29A80` | `0x29E70` | `0x29A1A`–`0x29E9A` |
| 4 | `0x29E9A` | 24 | `0x29EFC` | `0x29EFC` | `0x2A2C2` | `0x29E9A`–`0x2A2EC` (fim inferido) |

Total: **113 entradas**, o mesmo número documentado no checkpoint do jogador
para o mapa 0. As cinco tabelas são adjacentes: o fim de uma é o início da
seguinte, o que fixa a extensão das quatro primeiras. O banco 4 não tem tabela
seguinte; o script usa o tamanho modal do próprio banco (42 bytes) para a
última entrada, o que dá `0x2A2EC`. Isso não é arbitrário: a entrada
`0x2A2C2` termina em `0x2A2EA-0x2A2EB` com o mesmo final `83 00` das demais e
os bytes seguintes (`00 0A 00 1C 00 26 00 30 00 34 FF EF`) já são outra
estrutura. `0x2A2EC` também é o início do payload tipo 21 de `SEPL00`
registrado no checkpoint, o que serve de conferência independente.

Conferência contra o banco 3 conhecido: o script reencontra a tabela em
`0x29A1A` com 25 entradas, e a entrada 2 em `0x29AD4`, 42 bytes, SHA-256
`c39b9b3303df5000fd7a72ca85c3d3b61532fe32562006a09914c6f5629f4040`, igual ao
valor documentado.

## 2. Entradas por banco

Cada entrada é registrada com banco, número, endereço, tamanho, SHA-256 e os
bytes em hex (CSV e JSON). Histograma de tamanhos por banco:

| banco | tamanhos (bytes × quantidade) |
| --- | --- |
| 0 | 34×19, 40×1, 44×3, 60×1, 70×1, 80×1, 104×1, 130×1, 156×1, 248×1 |
| 1 | 34×12, 42×1 |
| 2 | 40×1, 42×20 |
| 3 | 42×25 |
| 4 | 42×24 |

Somando os cinco bancos: 34 B×31, 40 B×2, 42 B×70, 44 B×3, 60/70/80/104/130/
156/248 B×1 cada.

Assinatura estrutural: **113/113** entradas começam com os 8 bytes
`00 30 00 01 00 10 00 08` e **111/113** terminam com `83 00`. As duas
exceções são as entradas de 40 bytes `banco 0 seq 9` (`0x28F0C`) e
`banco 2 seq 6` (`0x297A6`), que terminam em `00 83`: são as versões truncadas
da entrada canônica de 42 bytes, e a tabela do driver realmente aponta a
entrada seguinte dois bytes antes. Isso confirma os limites de entrada
derivados das tabelas e, por consequência, as extensões de banco da seção 1.

## 3. Famílias de entradas quase idênticas

O agrupamento usa **conjunto exato de diferenças** contra uma referência (sem
encadeamento transitivo): dois registros só caem na mesma família se tiverem o
mesmo tamanho e diferirem da referência exatamente nos mesmos offsets. Com o
limite padrão de 6 offsets, há **71 famílias** com duas ou mais entradas
(36 de 2 membros, 21 de 3, 2 de 4, 2 de 12 e as grandes abaixo).

As maiores, e a generalização da família conhecida de 42 bytes:

| membros | tamanho | offsets que variam | valores observados |
| --- | --- | --- | --- |
| 70 | 42 | `0x0D`,`0x0E`,`0x0F`,`0x12`,`0x24` | `0x0D`∈{07,16}, `0x0E`∈{54,A1}, `0x0F`∈{20,76}, `0x12`∈{01,02,03,04}, `0x24`∈{00..18} |
| 45 | 42 | `0x12`,`0x24` | `0x12`∈{01,02,04}, `0x24`∈{00..17} |
| 30 | 34 | `0x0B`,`0x12`,`0x1C`,`0x1E` | `0x0B`=`0x1E`∈{14,1E,3C,50,5A,64,78,A0,B4,C8,F0}, `0x12`∈{00,01}, `0x1C`∈{00..16, sem 07} |
| 25 | 42 | `0x24` | `0x24`∈{00..18} (banco 3 inteiro) |
| 24 | 42 | `0x24` | `0x24`∈{00..17} (banco 4 inteiro) |
| 20 | 42 | `0x24` | `0x24`∈{00..14, sem 06} (20 das 21 do banco 2) |
| 19 | 34 | `0x0A`,`0x0B`,`0x1B`,`0x1C`,`0x1E` | ver JSON |
| 18 | 34 | `0x0B`,`0x1C`,`0x1E` | ver JSON |
| 15 | 34 | `0x12`,`0x1C` | banco 0 + banco 1, `0x12`∈{00,01} |
| 12 | 34 | `0x0B`,`0x1C`,`0x1E` | `0x0B`=`0x1E`∈{78,A0} |
| 12 | 34 | `0x0A`,`0x0B`,`0x12`,`0x1B`,`0x1C`,`0x1E` | ver JSON |
| 11 | 34 | `0x1C` | `0x1C`∈{01..0D, sem 05 e 07} (banco 1) |

Leituras que os bytes sustentam:

- Todas as **70 entradas de 42 bytes** dos bancos 1, 2, 3 e 4 são o mesmo
  modelo com apenas cinco posições variáveis. O campo `0x0D-0x0F` só aparece
  como `07 A1 20` ou `16 54 76`; o primeiro vale exatamente 500.000 em
  decimal. O campo `0x12` varia de 1 a 4. O byte `0x24` varia de 0 a 24.
- A família conhecida ("42 bytes, só `0x24` difere") aparece **três vezes**,
  uma por banco: banco 3 (`0x24` 0..24), banco 4 (`0x24` 0..23) e banco 2
  (`0x24` 0..20 sem o valor 6). As três são subconjuntos da família de 70
  membros; a de 25 membros é a que o checkpoint já documentava.
- Nas entradas de 34 bytes, `0x0B` e `0x1E` carregam **sempre o mesmo valor**
  nas 30 entradas da família maior, e os valores são `0x14, 0x1E, 0x3C, 0x50,
  0x5A, 0x64, 0x78, 0xA0, 0xB4, 0xC8, 0xF0` = 20, 30, 60, 80, 90, 100, 120,
  160, 180, 200, 240 em decimal, múltiplos de 10. Isso sugere um par
  nível/duração em décimos, mas o significado não foi verificado no driver.
- Nas entradas de 34 bytes o byte de índice é o `0x1C` (valores 0..0x16) e nas
  de 40 bytes é o `0x23` (as duas entradas de 40 bytes, `banco 0 seq 9` e
  `banco 2 seq 6`, diferem em `0x0D`,`0x0E`,`0x0F`,`0x12`,`0x23`). Ou seja,
  `0x24` é o índice específico do formato de 42 bytes.

## 4. Referências à janela do DSP no driver 68K

Região de código varrida: `0x1000`–`0x6800`. A imagem do driver tem 26.610
bytes (`0x67F2`), então tudo o que está abaixo disso pertence ao driver.

### 4.1 Inventário de cargas de ponteiro

Todas as cargas de imediato longo em registrador de endereço (`lea imm.l,An` e
`movea.l #imm,An`) foram inventariadas. As relevantes:

- `a5 ← 0x00100000` em 13 sítios (`0x1080`, `0x177E`, `0x21D6`, `0x22AA`,
  `0x23A8`, `0x240A`, `0x3184`, `0x330E`, `0x3866`, `0x389E`, `0x3F46`,
  `0x3FAA`, `0x5508`). Só existem outras duas cargas em `a5`: `0x0010000B`
  (`0x17BC`, dentro do laço que percorre slots com `adda.w d1,a5` em
  `0x17D4`) e `0x00009C00` (`0x5562`). Ou seja, `a5` é a base do SCSP em todo
  o driver.
- `a0 ← 0x00100800` (`0x5436`), `a0 ← 0x00100EC0` (`0x5478`),
  `a0 ← 0x00100EE0`/`0x00100EE2` (`0x532C`/`0x533A`),
  `a0 ← 0x00100ED2`/`0x00100ED4`/`0x00100ED6` (`0x5348`/`0x535A`/`0x536C`).
- `a1 ← 0x00100700` (`0x4BA8`), `a1 ← 0x00100000` (`0x53DE`),
  `a2 ← 0x00100000` (`0x5DDE`), `a2 ← 0x00004C20` (`0x4BAE`).
- `a0 ← 0x00100401` (`0x53F8`, `0x54B6`) e `a0 ← 0x00100408` (`0x17A8`):
  registradores globais do SCSP, não a janela do DSP.

### 4.2 Operandos absolutos de 32 bits

30 longwords no intervalo `0x00100700`–`0x00100EFF` em todo o dump. Desses,
10 estão alinhados na região de código:

| offset | valor | decodificação |
| --- | --- | --- |
| `0x04BAA` | `0x00100700` | `movea.l #$00100700,a1` (`227C`) |
| `0x0532E` | `0x00100EE0` | `movea.l #$00100EE0,a0` |
| `0x0533C` | `0x00100EE2` | `movea.l #$00100EE2,a0` |
| `0x0534A` | `0x00100ED2` | `movea.l #$00100ED2,a0` |
| `0x0535C` | `0x00100ED4` | `movea.l #$00100ED4,a0` |
| `0x0536E` | `0x00100ED6` | `movea.l #$00100ED6,a0` |
| `0x05438` | `0x00100800` | `movea.l #$00100800,a0` |
| `0x0547A` | `0x00100EC0` | `movea.l #$00100EC0,a0` |
| `0x02E56` | `0x00100C82` | não é operando: a metade alta é o deslocamento do branch em `0x02E54` |
| `0x05CDA` | `0x001008AE` | não é operando: a metade alta é o deslocamento do branch em `0x05CD8` |

Os outros 20 estão fora da região de código: 2 desalinhados dentro da imagem
(`0x05DA9`, `0x05F7D`) e 18 no meio dos dados de som (`0x0FACA`, `0x0FF99`,
`0x15C1F`, `0x2124E`, `0x32EF7`, `0x331B4`, `0x3515C`, `0x4695B`, `0x480A0`,
`0x4FF15`, `0x54ABF`, `0x5C227`, `0x5D1BB`, `0x5F42A`, `0x64CB0`, `0x6D62C`,
`0x6F0BE`, `0x7B92B`). São coincidências de quatro bytes em dados PCM: a taxa
observada (~1 a cada 26 KB) é a esperada para 2 bytes fixos seguidos de dois
bytes aleatórios, e nenhum deles está num fluxo de instruções.

### 4.3 Deslocamentos `d16(An)` em `0x700`–`0xEFF`

Onze referências decodificadas (todas `lea`), com a última carga explícita do
registrador-base antes da instrução:

| offset | forma | deslocamento | base | destino calculado |
| --- | --- | --- | --- | --- |
| `0x01338` | `41ED 0800` lea | `0x800` | `a5`=0x00100000 em `0x1080` | `0x00100800` (MPRO) |
| `0x01410` | `45ED 0EC0` lea | `0xEC0` | `a5`=0x00100000 em `0x1080` | `0x00100EC0` (EFREG) |
| `0x01446` | `47ED 0700` lea | `0x700` | `a5`=0x00100000 em `0x1080` | `0x00100700` (COEF) |
| `0x01A7A` | `41ED 0700` lea | `0x700` | `a5` com valor não demonstrável (`adda.w d1,a5` em `0x17D4`) | não demonstrável |
| `0x023AE` | `41ED 0700` lea | `0x700` | `a5`=0x00100000 em `0x23A8` | `0x00100700` (COEF) |
| `0x03FFC` | `43ED 0800` lea | `0x800` | `a5`=0x00100000 em `0x3FAA` | `0x00100800` (MPRO) |
| `0x04006` | `43ED 0780` lea | `0x780` | idem | `0x00100780` (MADRS) |
| `0x0400E` | `43ED 0700` lea | `0x700` | idem | `0x00100700` (COEF) |
| `0x04016` | `43ED 0C00` lea | `0xC00` | idem | `0x00100C00` (TEMP) |
| `0x05DE4` | `49EA 0700` lea | `0x700` | `a2`=0x00100000 em `0x5DDE` | `0x00100700` (COEF) |
| `0x05DE8` | `47EA 0780` lea | `0x780` | `a2`=0x00100000 em `0x5DDE` | `0x00100780` (MADRS) |

Uma única referência genérica aparece em `0x05FD8` (palavra `0x802E`,
deslocamento `0x0806`), mas ela é a **palavra de extensão** de
`move.b (d8,pc,Xn),d0` em `0x05FD6` (`103B 802E`), não uma instrução. A
varredura deslizante não consegue excluir esse tipo de artefato, então o
script a marca como não confirmada; nenhuma outra apareceu.

Observação sobre `0x05DE4`: `a2` é imediatamente recarregado de `(0x412).w`
em `0x05DEC`, mas `a4` e `a3` já foram calculados a partir de `0x00100700` e
`0x00100780`, e é neles que o resto da rotina escreve, com deslocamentos fixos
`0x5A` e `0x4C` a partir de `a4 = 0x00100700 + (byte em 6(a2))`. Com offset 0
isso dá `0x0010075A` e `0x0010074C`; para qualquer offset menor que `0xA6`
continua dentro de COEF. As rampas de coeficientes vem das tabelas em `0x5E94`
e `0x5F36`.

### 4.4 A constante de partida do DSP `0x00100BF0`

O longword `0x00100BF0` **não aparece em nenhum offset dos 524.288 bytes**. A
palavra isolada `0x0BF0` aparece 15 vezes; só uma está na imagem do driver:

```
0x04048  2E3C 0000 0BF0   move.l #$00000BF0,d7
0x0404E  7000             moveq #0,d0
0x04050  204E             movea.l (a6),a0
0x04052  20C0             move.l d0,(a0)+
0x04054  51CF ....        dbf d7,0x4052
```

Ou seja, `0xBF0` ali é a **contagem** de um laço que grava zero no buffer
apontado por `(a6)` (12.224 bytes), não o registrador de partida do DSP. As
outras 14 ocorrências estão nos dados de som
(`0x13DD6`, `0x2B7D8`, `0x32690`, `0x34CE6`, `0x3B73E`, `0x41E2E`, `0x4215C`,
`0x5A074`, `0x5EF46`, `0x5F306`, `0x615AC`, `0x643DA`, `0x7C7D6`, `0x7CCFC`),
todas em offsets pares dentro de PCM e sem instrução associada.

A partida do DSP, portanto, só pode acontecer pelo mecanismo do próprio chip:
escrever a palavra de offset `0xBF0` da janela MPRO (`0x00100BF0`). O driver
faz isso **de forma implícita**, porque seus laços de limpeza cobrem
`0x700`–`0xBFF` inteiro e, ao escrever `0xBF0` com zero, armam o DSP com
programa vazio (`lastStep = 0`, nenhum passo executado).

### 4.5 O único caminho que grava dados não nulos em `0x800`–`0xBFF`

Todo o resto do driver escreve zero na janela do DSP. Existe **um** caminho que
copia dados de um buffer para `0x00100700`–`0x00100BFF`:

```
0x013C6  206E 1858   movea.l (0x1858,a6),a0     ; a0 = [0x8858]
0x013CA  246E 1850   movea.l (0x1850,a6),a2     ; a2 = [0x8850]
0x013CE  262E 1854   move.l  (0x1854,a6),d3     ; d3 = [0x8854]
0x013D2  203C 60006000                         ; move.l #$60006000,d0
0x013D8  E88B                                   ; lsr.w #4,d3
0x013E2  24C0                                 ; move.l d0,(a2)+   (4x)
0x013EA  51CB FFF6                            ; dbf d3,0x13E2
0x013EE  1028 0023   move.b (0x23,a0),d0
0x013F2  11C0 0484   move.b d0,$0484.w
...
0x01410  45ED 0EC0   lea (0xEC0,a5),a2         ; a2 = 0x00100EC0
0x01414  7000                                 ; moveq #0,d0
0x01416  24C0                                 ; move.l d0,(a2)+   (8x: limpa 0xEC0-0xEDF)
0x01426  2A2E 1850   move.l (0x1850,a6),d5
0x0142A  1028 0020   move.b (0x20,a0),d0
0x0142E  0240 0003                            ; andi.w #3,d0
0x01432  EF48                                 ; deslocamento de d0 (campo RBP)
0x01434  E78D                                 ; deslocamento de d5
0x01436  4845                                 ; swap d5
0x01438  8045                                 ; or.w d5,d0
0x0143A  3B40 0402   move.w d0,(0x402,a5)      ; RBP/RBL do SCSP
0x0143E  3D40 1808   move.w d0,(0x1808,a6)     ; cache em 0x8808
0x01442  45E8 0040   lea (0x40,a0),a2          ; fonte = [0x8858] + 0x40
0x01446  47ED 0700   lea (0x700,a5),a3         ; destino = 0x00100700
0x0144A  3E3C 0004   move.w #4,d7
0x0144E  26DA        move.l (a2)+,(a3)+         ; 64 instruções desenroladas
0x014CE  51CF FF7E   dbf d7,0x144E
```

`0x144E`–`0x14CE` tem 64 `move.l (a2)+,(a3)+` (256 bytes) e o `dbf` em
`0x14CE` repete 5 vezes: **1280 bytes** copiados de `[0x8858]+0x40` para
`0x00100700`–`0x00100BFF`. O destino cobre exatamente

```
0x700-0x77F  128 bytes  COEF
0x780-0x7BF   64 bytes  MADRS
0x7C0-0x7FF   64 bytes  (espelho de MADRS; escritas ignoradas)
0x800-0xBFF 1024 bytes  MPRO (programa de 128 passos)
             ----
             1280 bytes
```

com `a5 = 0x00100000`: no trecho `0x1000`–`0x1446` não há nenhuma outra
escrita em `a5` além de `2A7C 00100000` em `0x1080` (as 13 cargas explícitas
de `a5` na imagem valem `0x00100000`, exceto `0x0010000B` em `0x17BC`, que é
posterior).

O ponteiro de origem vem da área de trabalho do driver (`a6 = 0x00007000`,
`2C7C 00007000` em `0x1086`), logo `0x1858+0x7000 = 0x8858`. Quem escreve esse
ponteiro é o interpretador de comandos em `0x33xx`:

```
0x033C0  08B8 0001 0422   bclr #1,$0422.w
0x033C6  2018             move.l (a0)+,d0
0x033C8  0280 000FFFFF    andi.l #$000FFFFF,d0   ; offset de 20 bits
...
0x033DE  2040             movea.l d0,a0
0x033E0  2D40 1858        move.l d0,(0x1858,a6)  ; [0x8858] = offset da imagem
...
0x03438  2D45 1850        move.l d5,(0x1850,a6)  ; [0x8850] = destino/limite
0x0343C  2D43 1854        move.l d3,(0x1854,a6)  ; [0x8854] = comprimento
```

Nos dois dumps, `[0x8858] = [0x8850] = [0x8854] = 0`, e o cache de RBP em
`0x8808` também é zero. A área de trabalho inteira `0x7000`–`0x8838` está
zerada nas duas capturas (inclusive o contador `0x1832` que dispara o bloco em
`0x1330`), enquanto o mailbox `0x700`–`0x7A0` tem pacotes pendentes. Isso é
coerente com um driver recém-reiniciado que ainda não processou o comando que
aponta a imagem de DSP.

### 4.6 Todas as demais escritas na janela são zerofill

| sítio | instrução | destino | bytes | efeito |
| --- | --- | --- | --- | --- |
| `0x1338` | `41ED 0800` + `7000` + `3E3C 0005` + 64×`20C0` + `dbf` (`0x13C2`) | `0x00100800` | 6×256 = 1536 | limpa `0x800`–`0xDFF` (MPRO + faixa TEMP, cujas escritas o chip ignora) |
| `0x1A7A` | `41ED 0700` + `3E3C 013F` + `20C0` + `dbf` (`0x1A86`) | `0x700` + `a5` | 0x140×4 = 1280 | limpa `0x700`–`0xBFF` |
| `0x23AE` | `41ED 0700` + `3E3C 013F` + `20C0` + `dbf` (`0x23BA`) | `0x700` + `a5` | 0x140×4 = 1280 | limpa `0x700`–`0xBFF` |
| `0x3FFC` | despacho: `43ED 0800`/`00FF`, `43ED 0780`/`1F`, `43ED 0700`/`1F`, `43ED 0C00`/`7F` + `7000` + `22C0` + `dbf` (`0x4020`) | MPRO/MADRS/COEF/TEMP | 1024/128/128/512 | limpa a região escolhida |
| `0x5436` | `207C 00100800` + `7200` + `20C1`×2 + `dbf` (`0x5440`) | `0x00100800` | 128×8 = 1024 | limpa MPRO |

Todas usam registrador zerado como fonte (`d0` ou `d1`, ambos com `moveq #0`).
O `0x543C` (`20C1`) é `move.l d1,(a0)+` com `d1 = 0` (`7200` em `0x5434`),
portanto também é zerofill.

## 5. Varredura heurística de programas de DSP

O SCSP **não** mapeia a área MPRO em RAM de som: `0x800`–`0xBFF` é um arquivo
de registradores dentro do chip. Um dump de RAM não pode conter "o programa em
MPRO"; o que ele pode conter é um bloco de dados que, lido como 128 passos de
4 palavras, respeite as faixas de campo. A varredura abaixo é, portanto,
heurística:

- bits reservados zerados (bit 15 da palavra 0 e bit 12 da palavra 1),
- `IRA <= 0x31` em todos os passos não nulos (valores maiores fazem o DSP
  abandonar a amostra),
- passos não nulos contíguos a partir do passo 0,
- para o critério "estruturado": pelo menos 2 acessos à linha de atraso
  (`MRD`/`MWT`), pelo menos 1 escrita de efeito (`EWT`) e `NXADR` no último
  passo.

Resultados nos dois dumps (idênticos):

- **Imagem do driver, com o bloco de 1 KB inteiro dentro de `0x0000`–`0x6800`:
  0 candidatos estruturados.** Há 255 blocos que passam só na validade de
  campo, e todos os 255 começam em `0x0000`–`0x0800`, nas tabelas de
  ponteiros/vetores (palavras altas `0x0000`). Só um deles tem acesso à linha
  de atraso: `0x0043E` (4 passos, sem `NXADR`), coincidência na tabela de
  vetores.
- **Dump inteiro: 4 candidatos estruturados**, todos fora da imagem do driver
  ou straddling o fim dela: `0x067D2` (4 passos; começa 32 bytes antes de
  `0x67F2`, o fim do driver, e o bloco termina nos dados de som), `0x11CB2` (8
  passos), `0x52EBA` (5 passos) e `0x52EC2` (4 passos). São blocos de 1 KB em
  PCM com aparência de programa; nenhum tem 128 passos nem vem de uma cópia
  para `0x00100800`.
- 250 blocos "relaxados" começam fora da imagem do driver; nenhum deles é
  reivindicado como programa.

Conclusão desta seção: **não há programa de DSP embutido na imagem do driver**.
A varredura não encontrou nenhum bloco de 1 KB com os critérios estruturados
inteiramente dentro de `0x0000`–`0x67F2`.

## 6. Veredito: o driver carrega programa de DSP?

O driver **tem** um caminho de carga de imagem de DSP, e ele é o único que
grava dados não nulos na janela: `0x13C6`–`0x14D2` copia 1280 bytes de
`[0x8858]+0x40` para `0x00100700`–`0x00100BFF` (4.5). O ponteiro `0x8858` é
preenchido pelo interpretador de comandos em `0x33E0`, a partir de um offset
de 20 bits do fluxo de comandos (`0280 000FFFFF` em `0x33C8`).

**Nas duas capturas, esse caminho não carregou imagem alguma.** As evidências
estão nos bytes:

- `[0x8858]`, `[0x8850]`, `[0x8854]` e o cache `[0x8808]` valem **zero** nos
  dois dumps; a área `0x7000`–`0x8838` está toda zerada.
- Fora desse caminho, todas as escritas em `0x800`–`0xBFF` são zerofill
  (4.6), e `0x00100BF0` não existe em lugar nenhum do dump (4.4). O DSP é
  armado implicitamente pelas limpezas, com programa vazio.
- A imagem do driver não contém programa embutido (5): 0 candidatos
  estruturados em `0x0000`–`0x67F2`.
- A execução dinâmica no probe registrada em `SCSP-DRIVER-PROBE.md` (10M
  ciclos, `--replay-pending --mark-loaded`) viu 1024 escritas na janela do DSP,
  **todas com valor zero**, `dsp_steps=0`, `noise_samples=0`,
  `pitch_lfo_samples=0` e `fm_samples=0`. Isso é independente desta análise
  estática e concorda com ela.

Portanto: para estas capturas, o `SDDRVS.TSK` **não carrega programa de
efeito**; ele apenas limpa a área do DSP e, no máximo, atualiza coeficientes
(`0x4BA8` para COEF, `0x5DE4`/`0x5DE8` para COEF/MADRS) e o RBP/RBL em
`0x00100402` (`0x143A`). Sem programa em MPRO, `lastStep = 0` e nenhum passo
do DSP executa, de modo que coeficientes e endereços não produzem efeito
audível.

O que a evidência **não** prova:

- Não prova que o jogo nunca envia o comando de imagem de DSP. O caminho existe
  e depende de um offset de 20 bits vindo do fluxo de comandos; outra fase,
  outro efeito ou um comando `8A` com pedido de DSP pode apontá-lo. As duas
  capturas são dois instantes, não uma cobertura de todos os estados.
- Não prova o conteúdo nem a validade da imagem que o jogo apontaria: com
  `[0x8858] = 0`, não há imagem localizável no dump para inspecionar. A
  varredura da seção 5 só pode dizer que essa imagem não está na imagem do
  driver.
- Não prova que `0x13C6`–`0x14D2` seja alcançado em gameplay. O bloco é
  precedido pelo contador `0x1832` (`532E 1832` em `0x1330`), que está zerado
  nas capturas; o disparo depende do comando que também preenche `0x8858`.
- Não decodifica as sequências (seção 2/3) nem liga nomes de efeito (tiro,
  dash, WARNING) a entradas ou a comandos.
- Não substitui a validação dinâmica: se um dia o comando de imagem aparecer,
  é o probe que dirá se o DSP executou passos (`dsp_steps > 0`) e se o áudio
  mudou.

Uma consequência prática para o emulador: não é preciso embutir um programa de
DSP fixo do X4. Se o jogo enviar a imagem, ela vem apontada por um offset nos
dados de som e o caminho a percorrer é o mesmo que o driver real usa
(`0x144E`). O que falta para os efeitos de gameplay continua sendo decodificar
as sequências e os comandos, não o DSP.

## Artefatos

| arquivo | conteúdo |
| --- | --- |
| `build-scsp/tmp-test/sequence-analysis/report-both.txt` | saída completa das seções 1 a 6 para os dois dumps |
| `build-scsp/tmp-test/sequence-analysis/scsp-driver-analysis.json` | candidatos de banco, 113 entradas, 71 famílias, referências ao DSP e candidatos de programa |
| `build-scsp/tmp-test/sequence-analysis/scsp-driver-entries.csv` | as 113 entradas com endereço, tamanho, SHA-256 e bytes |
| `tools/diagnostics/analyze_x4_scsp_driver.py` | a ferramenta que gera os três anteriores |
