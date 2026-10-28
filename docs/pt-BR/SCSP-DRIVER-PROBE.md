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

DSP, FM, LFO e noise não são sintetizados. Os pedidos não suportados são
contabilizados por slot/amostra; noise produz silêncio. A execução do 68000
ocorre em lotes durante o VBlank do runtime, portanto a interação com pedidos
SH-2 ainda tem granularidade de um frame, em vez de ciclo a ciclo. Os sons
específicos de gameplay e o sincronismo precisam de validação na fase com o
debugger/OBS; não estão todos declarados corrigidos.
