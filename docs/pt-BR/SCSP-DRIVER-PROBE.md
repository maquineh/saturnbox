# Driver SCSP e síntese PCM

Foi acrescentado um caminho opcional para executar instruções reais do
68000 com Musashi. O probe permite testar o driver isoladamente; o runtime
também pode selecionar esse backend com `--scsp-driver`, usando o mesmo
sintetizador PCM. O caminho HLE continua sendo o padrão.

```powershell
cmake -S . -B build-scsp -DSATURNBOX_MUSASHI_68K=ON -DSATURNBOX_VULKAN=OFF
cmake --build build-scsp --config Release --target saturn-scsp-probe saturnbox-tests
```

O módulo obtém Musashi no commit
`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`. O build padrão mantém essa opção
desligada e não baixa a dependência. As licenças Musashi e SoftFloat estão em
`third_party/`. No MSVC 19.44 usado nesta máquina, o gerador de opcodes precisa
de `/Od`: com `/O2`, o binário gerado omitia o bit `0x4000` das máscaras,
produzindo uma tabela inválida. Apenas o gerador foi desotimizado.

Uso do probe:

```powershell
build-scsp/tools/saturn-scsp-probe/Release/saturn-scsp-probe.exe `
  build/rockman/SDDRVS.TSK 1000000 build/tmp/scsp-register-writes.csv
```

Ele aceita o driver ou um dump completo de RAM SCSP, com vetores de reset no
início. Os drivers/recursos extraídos do disco não são incluídos no código.
O orçamento máximo aceito é 1 bilhão de ciclos. O backend usa estado global
e permite uma instância ativa por processo. Callbacks de barramento são
big-endian, com acessos de 8/16/32 bits. O custo de reset também entra na
contagem de ciclos.

Os timers avançam pelo acumulado real de ciclos: 256 ciclos do 68000 por
amostra a 44,1 kHz, preservando o resto após instruções que excedem o orçamento.
Essa razão confere com a [configuração Saturn do MAME](https://github.com/mamedev/mame/blob/master/src/mame/sega/sat_console.cpp#L776).
Prescalers, pending, máscara, SCIRE e níveis SCILV seguem o recorte descrito
nos manuais Sega de [timers](https://www.infochunk.com/saturn/segahtml_en/hard/scsp/hon/p04_2b.htm)
e [interrupções](https://www.infochunk.com/saturn/segahtml_en/hard/scsp/hon/p04_2c.htm).
Saturação do timer em FFh até recarga segue o MAME; o manual não esclarece
wrap versus parada. Reassert de SCIRE sobre timer já expirado permanece
pendente de validação. Interrupção da CPU principal, MIDI e DSP não são
implementados neste probe.

## Resultado de 27/10/2026

Com `SDDRVS.TSK` de 26.610 bytes, vetores SP=`0xA000`, PC=`0x1000`:

- Sem timers: cerca de 1M ciclos, PC final `0x10EC`, 789 escritas em
  registradores, 320 em slots, zero acessos fora do mapa.
- Com timers: 1.000.012 ciclos, PC final `0x506E`, 914 escritas, 42 eventos de
  timer B, zero acessos fora do mapa. O driver recarrega o timer e limpa IRQ.
- `--test-master-volume`, depois do caminho do CSV, publica um pedido `0x82`
  após a inicialização. O driver real mudou MVOL para 5 e limpou o handshake;
  o teste passou.
- `--replay-pending` reapresenta os oito pacotes do dump de RAM de entrada após
  a inicialização. Com o snapshot de gameplay anterior, 2M ciclos terminaram
  com o handshake limpo, 384 escritas em slots e **zero pedidos de key-on**.
  A confirmação do lote não prova reprodução de uma sequência ou efeito.

Artefatos: `build/artifacts/scsp-driver-probe-20261027/`. Os testes incluem
instruções reais, larguras de barramento, reset fracionado, IRQ e ciclo de
vida do core, além dos timers e do controlador de interrupções.

## Primeira implementação de slots e integração no runtime

`ScspSynth` implementa 32 slots, PCM8/PCM16 assinado em big-endian, endereço
SA, limites LSA/LEA, os quatro modos de loop, pitch OCT/FNS e interpolação.
Inclui key-on/off global por KYONEX/KYONB, volume TL/DISDL/MVOL, pan DIPAN,
envelopes attack/decay1/decay2/release e monitor CA/SGC/EG selecionado por
MSLC. Tabelas e equações derivadas do MAME preservam o aviso BSD-3-Clause
em `third_party/MAME-SCSP-LICENSE.txt`.

No modo novo, SMPC SNDON/SNDOFF controla o reset/execução do 68000. Ele usa
os vetores da RAM e avança 256 ciclos por amostra, carregando excedentes de
instrução. As escritas do driver chegam aos slots e timers; o áudio gerado
vai ao frontend/WAV. O HLE **não** limpa o handshake dos pacotes nesse modo.
O mailbox específico do jogo em WRAM ainda mantém seu serviço HLE anterior.

Para testar em uma build com `SATURNBOX_MUSASHI_68K=ON`:

```powershell
.\tools\diagnostics\launch_x4_debug.ps1 -ScspDriver
```

A build local em `build` foi recompilada com o core disponível. O executável
recusa `--scsp-driver` com mensagem clara quando esse suporte não foi compilado.
As coletas do debugger incluem PC/ciclos e contadores de key-on e recursos
de áudio não implementados. Vetores de reset inválidos produzem diagnóstico
de falha, sem confirmar os pedidos por HLE.

### Diagnóstico do flag de transferência L

O dump antigo tinha dados presentes, mas o driver recém-inicializado criava
a tabela CRNT sem o bit L, que indica transferência concluída. O
[guia Sega do mapa](https://www.infochunk.com/saturn/segahtml_en/sndt/prog/drve/sndctrl.htm)
descreve esse bit. No replay comparável de 10M ciclos, sem L o bitmap de erro
do driver na **RAM de som** `0x420` era `0x104` e não havia vozes. Com L,
após validar os dez descritores conhecidos do dump X4, o erro caiu para zero,
o driver acionou duas vozes e gerou WAV não silencioso. Esse endereço de RAM
não é o registrador SCIPD em `0x100420` do barramento 68000.

O probe oferece `--mark-loaded`, somente com `--replay-pending`, para montar
essa fixture explícita. Ele verifica os descritores e seus limites; depende
de o dump fornecido conter os dados correspondentes. **O runtime não marca L
automaticamente.** Na execução pela ISO, o fluxo original deve completar a
transferência depois de consultar as estruturas criadas pelo driver real.

O resultado final da fixture teve 10.000.000 ciclos, dois key-ons sintetizados,
duas vozes ativas, erro zero, zero acessos fora do mapa e 39.062 frames de áudio
a 44,1 kHz. Não usou DSP/LFO e não houve amostras fora de faixa. Artefatos em
`build/artifacts/scsp-driver-mode-20261027/`, incluindo `sequence-final.wav`.
Esse caso confirma reprodução de uma sequência carregada; não identifica
essa sequência como tiro, WARNING ou explosão.

O boot real pela ISO, sem `--mark-loaded`, também chegou a 25M blocos com
handoff e `step-limit`: 334.548.486 ciclos do 68000, duas inicializações,
27 key-ons/23 key-offs, erro SDK zero e zero acessos ou amostras fora do mapa.
O fluxo original concluiu o carregamento dos bancos. O WAV desse teste tem
29,75 segundos a 44,1 kHz estéreo e contém áudio não silencioso. A captura
em 24M e os dumps estão em `build/artifacts/scsp-driver-boot25m/`; manifest e
log em `build/tmp/scsp-driver-boot25m.*`. Isso cobre boot/áudio inicial,
não o combate na fase.

### Limites do recorte

DSP, FM, LFO e noise passaram a ser sintetizados em 28/10 (ver abaixo). O
controle MIDI e a DMA do SCSP continuam fora do modelo. A execução do 68000
avança a cada amostra de 44,1 kHz desde a versão anterior; o que mudou agora é
o **quando** o relógio de áudio avança no runtime, não a razão ciclos/amostra.
Os sons específicos de gameplay e o sincronismo ainda precisam de validação
audível na fase com o debugger; os testes provam a síntese, não a fidelidade
de um efeito nomeado.

## Efeitos, vozes e sincronismo (28/10/2026)

Esta etapa acrescenta as fontes de som que o modelo anterior contava como
ausentes e muda o relógio de áudio para o mesmo quantum do scheduler.

### Noise, LFO e modulação de fase

- **Noise (`SSCTL=1`)**: `ScspSynth::nextNoise` usa um xorshift de 16 bits
  determinístico, reiniciado com o chip. O manual Sega não documenta os taps do
  LFSR e o MAME usa o RNG do host como aproximação; aqui o objetivo é ser
  reprodutível entre execuções. Os bits de inversão `SBCTL` continuam aplicados.
  `SSCTL=2` permanece silêncio especificado e `SSCTL=3` continua contado em
  `unsupportedSourceSamples` por ser marcado como inutilizável pelo manual.
- **LFO**: `PLFOS`/`ALFOS`, as quatro formas (saw, square, triangle, noise), as
  32 taxas, os 494 cents de profundidade de pitch e os 24 dB de amplitude vêm do
  MAME. O passo de fase inteiro trunca as taxas mais baixas (0,17–0,35 Hz) para
  zero, como no MAME; essa limitação foi preservada de propósito. `LFORE`
  reinicia a fase no key-on e, sem ele, o oscilador continua livre.
- **Modulação de fase (FM)**: `MDL`/`MDXSL`/`MDYSL` leem o ring buffer de 64
  entradas com as saídas recentes dos slots, atenuadas por TL como no MAME, e
  somam o desvio ao endereço de leitura. O acumulador de posição e os limites
  `LSA`/`LEA` não são afetados, então o loop continua coerente.

### DSP de efeitos

`libs/saturn/src/scsp_dsp.cpp` porta o `scspdsp.cpp` do MAME: programa de 128
passos em 24 bits, 64 coeficientes (`0x700`), 32 palavras de endereço
(`0x780`), programa (`0x800`–`0xBFF`), linha de atraso em 128 posições e 16
registradores de efeito. A escrita em `0xBF0` deixa o DSP fora de `stopped` e
recalcula o último passo não nulo do programa. `TEMP`, `MEMS`, `MIXS`, `EFREG` e
`EXTS` voltam por leitura em `0xE00`–`0xEE3`.

A memória de atraso fica na **RAM de som**, endereçada por `RBP`/`RBL`
(`0x402`), e por isso `ScspSynth::renderSample` agora recebe a RAM como
gravável. O caminho de ganho segue o MAME: envio por slot
`amostra16 × 16 × TL × IMXL`, acumulado em `MIXS[ISEL]`, e retorno dos
`EFREG[0..15]` somado à saída pelos `EFSDL`/`EFPAN` dos slots 0–15. Quando a
imagem de RAM for menor que 512 KiB, o DSP limita os acessos ao tamanho real e
conta as escritas descartadas em `droppedWrites`; sem isso, um endereço de
atraso poderia sair do buffer nos testes.

### Sincronismo por passo do scheduler

Antes, `onVBlank` renderizava as 735 amostras de um quadro de uma vez. Isso
significava que todo o 68000 de um quadro rodava em um único bloco, e que toda
escrita da SH-2 em registradores de slot só era vista no quadro seguinte.

Agora `ScuEventScheduler::beforeBlock` informa `setAudioFrameBlocks` e chama
`onBlockTick` no mesmo ponto em que conta o tempo do quadro. `onBlockTick`
renderiza a fração de amostras devida por passo (`735 / passos-por-quadro`) em
um buffer de quadro, e `onVBlank` completa e publica o quadro. Com o padrão de
14.000 passos por quadro, cada passo renderiza cerca de 0,05 amostra, então a
ordem entre execução do 68000, comandos da mailbox e escritas de registrador da
SH-2 passa a ser sub-quadro em vez de um quadro inteiro. O clock em tempo real
continua seguindo o tempo do host e ignora esse caminho.

A contagem continua em 735 amostras por quadro e os testes existentes de
`onVBlank` seguem passando porque o caminho antigo permanece quando nenhum
passo por quadro é configurado.

Contadores novos em `ScspRuntime::Stats`: `noiseSamples`, `pitchLfoSamples`,
`amplitudeLfoSamples`, `fmSamples`, `dspSendSamples`, `dspEffectSamples`,
`dspSteps`, `audioBlockTicks`, `audioSubFrameSamples` e `slotMask` (um bit por
slot soando, para separar música, efeito e voz). O debugger ao vivo e o probe
publicam todos.

### Testes desta etapa

`tests/scsp_synth_tests.cpp` e `tests/scsp_driver_tests.cpp` cobrem: noise
audível e `SSCTL=3` silencioso; LFO de pitch alterando a posição e LFO de
amplitude variando o nível; modulação de fase pelo ring buffer; envio ao DSP com
retorno em `EFREG` (saída apenas pelo efeito, com `DISDL=0`); escrita da linha de
atraso em `word 0x100` da RAM de som; e o relógio de áudio adiantando amostras
dentro do quadro, com o key-on feito no meio dele. No modo `--scsp-driver`, um
teste adicional confirma que o 68000 executa durante os passos, não só no
VBlank.

### Execução no probe com dados reais (28/10/2026)

Com o dump `scsp_ram.bin` da captura de fase e `--replay-pending --mark-loaded`,
10 milhões de ciclos reproduziram a fixture conhecida: 2 key-ons, 2 vozes ativas,
bitmap de erro do driver em `0x420` igual a zero, 39.062 amostras e um WAV com
pico de 22.373 (68% da escala) e RMS de 9,89% da escala, portanto não silencioso.

Nessa execução o driver fez 1.024 escritas na janela do DSP, **todas com valor
zero**, e nenhum slot usou noise, LFO ou modulação: `dsp_steps=0`,
`noise_samples=0`, `pitch_lfo_samples=0`, `fm_samples=0`. O `SDDRVS.TSK` desta
captura limpa a área do programa do DSP, mas não carrega um programa de efeito, e
as vozes observadas vêm de slots PCM diretos. Isso delimita a conclusão: o DSP
está implementado e coberto por testes sintéticos, porém o áudio capturado do X4
não depende dele. O que falta para tiro, explosão e voz não é o DSP, e sim
decodificar e acionar as sequências corretas (comando `01` e eventos comprimidos)
e validar o som na fase.

### Pendências

- Identificar o programa de DSP que o driver `SDDRVS.TSK` realmente carrega e se
  os efeitos/vozes do X4 passam por ele ou só pelos slots diretos.
- Decodificar os eventos das sequências e confirmar auditivamente tiro, dash,
  WARNING e explosão na fase.
- Medir se noise/LFO aparecem nos bancos usados pelo jogo, em vez de só nos
  testes sintéticos.
- O interleaving ainda é por passo do scheduler, não por ciclo do 68000; a
  granularidade é muito menor que um quadro, mas não é ciclo a ciclo.
- MIDI e DMA do SCSP continuam não implementados.
