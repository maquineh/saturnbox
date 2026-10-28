# Execução isolada do driver SCSP

Foi acrescentado um caminho opcional para executar instruções reais do
68000 com Musashi, sem ligar esse experimento à mixagem HLE usada no jogo.
Isso permite testar o driver carregado e seus pedidos antes de substituir o
caminho de áudio atual.

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

Ainda faltam síntese real dos slots, envelopes, laços, DSP e integração do
core com SMPC/reset e com o fluxo do jogo. Os sons de tiro/WARNING/explosão
**não estão declarados corrigidos** por esses resultados.
