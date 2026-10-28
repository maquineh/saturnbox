# Rockman X4 — checkpoint de teste do jogador

## Estado

Atualização de 27/10, após o teste ao vivo: o usuário concluiu a fase inicial
e avançou pela fase de Frost Kibatodos. Movimento e combate com controle ao
vivo foram gravados. Duplicações de poses, corrupção listrada, oclusão e
ausência de efeitos sonoros estão documentadas no
[diagnóstico da sessão](ROCKMAN-X4-LIVE-GLITCH-AUDIT.md), com distinção entre
sintomas confirmados, lacunas do código e causas ainda não comprovadas.
As entradas abaixo preservam o histórico anterior.

Na continuação dessa sessão, a limpeza/troca VDP1 foi corrigida e validada com
replays antes/depois. O [debugger ao vivo](LIVE-DEBUGGER.md) permite novas
capturas por F8 ou pedido externo durante a jogatina. O
[probe SCSP](SCSP-DRIVER-PROBE.md) já executa o driver real com IRQs de timer e
processa um pedido de volume; a síntese dos efeitos sonoros ainda está pendente.

Atualização de 19/10: a rota curta seleciona X, pede `ST00_0_X.BIN` em FAD
12965 e chega à primeira cena da fase. O replay corrigido terminou em 108M,
com captura em 106M: X aparece azul e íntegro. Os 2.304 bytes da textura
coincidem integralmente entre a descompressão independente de `PL00.ARC`,
a RAM e o VDP1. A trilha confirma o DMA completo dessa pose.

A origem do caso de `0x780` bytes da iteração 41 também foi fechada:
o descompressor SH-2 `0x06016F98`, executado pelo escravo, lê o tipo 2 de
`PL00.ARC+0x380E` e escreve `0x06003000`. Foram corrigidos os aliases de
WRAM e o serviço cooperativo de FRT; não houve alteração do renderer.

A duração do trabalho escravo e a contenção entre CPUs ainda usam um recorte
HLE por eventos. O caminho do botão direita até a seleção de velocidade foi
confirmado em runtime, mas deslocamento contínuo, outras ações e troca de
overlays durante a fase ainda não foram validados. As capturas históricas de
movimento de 18/10 permanecem como evidência separada.

A imagem fornecida pelo usuário é um quadro da abertura e foi tratada como
conteúdo esperado, não como evidência de falha gráfica.

## Alterações no código

- `saturn-sh2 boot` aceita `--pad-sequence` com eventos de controle
  `blocos:botões`, separados por ponto e vírgula. Use vírgula entre botões e
  `release` para soltar todos. Exemplo: `100000:right;200000:right,b;300000:release`.
- Com uma sequência configurada, a entrada do teclado/janela não sobrescreve o
  controle 1 scripted.
- `LaunchMemoryProbe::backingAddress()` encaminha a consulta ao serviço de boot
  para preservar os aliases de RAM usados pelo jogo durante o tracing.

## Iterações e evidências

- Iteração 38: primeira captura limpa da sala de transição, com X à esquerda;
  `build/artifacts/player-test-iter38-finish-opening/captured-frame.png`.
- Iterações 39 e 40: apertar e manter direita, inclusive por 50 milhões de
  blocos, não mudou a imagem da iteração 38. A sala ainda não era uma partida
  controlável ou faltava avançar o fluxo de entrada.
- Iteração 41: sequência de Start/direcional/confirmação alcançou uma cena de
  fase após carregar novos dados do CD. O quadro mostra X perto do centro, mas
  com corrupção vermelha e verde; VDP1 reportou 60 comandos, dos quais 35
  sprites e 5 polígonos. Captura: `build/artifacts/player-test-iter41-start-stage/captured-frame.png`.
  Recorte: `build/artifacts/player-test-iter41-start-stage/player-crop.png`.
  Os diagnósticos por camada e os dumps de VRAM/CRAM ficam em
  `build/artifacts/player-test-iter41-start-stage/video-diagnostics/`.

Na iteração 41, a captura foi armada no bloco 490.000.000 e a execução terminou
em 510.000.000. A sequência de controle usada foi:

```text
380000000:start
385000000:right
400000000:a
420000000:right
432000000:a
450000000:right
470000000:right,b
495000000:release
```

O resumo da execução informa que o CD continuou lendo setores e que a execução
terminou por limite de passos. A captura isolada não prova deslocamento de X.

- Iteração 46: repetiu a sequência com `--vblank-blocks 25000` e registrou as
  escritas na memória de textura VDP1. O SCU DMA0 usou a origem `0x06003000`.
  A captura da iteração 46
  (`build/artifacts/player-test-iter46-jit-vdp1-writers/captured-frame.png`)
  ficou só com o céu e nenhum sprite VDP1. Embora o replay tenha chegado ao
  mesmo bloco final, o FAD final do CD foi 65241, contra 12965 na iteração 41;
  portanto, a cena do glitch não foi reproduzida.

  O CSV `build/artifacts/player-test-iter46-jit-vdp1-writers/video-diagnostics/frame_360703_vdp1-memory-writes.csv`
  registra a configuração observada: `R2=0x25FE0010` (controle DMA0),
  `R3=0x101` (habilita/inicia DMA0) e `R5=0x86003000` (alias da origem
  `0x06003000`).

  A captura corrompida da iteração 41 fecha a ligação. Os comandos VDP1 155–159
  leem cinco tiles 4-bpp de 128 bytes em `0x60080`, `0x60100`, `0x60180`,
  `0x60200` e `0x60280`; o DMA copia `0x780` bytes, e todo o destino
  `0x60000–0x6077F` coincide com a faixa RAM `0x06003000–0x0600377F`.
  Desses 1.920 bytes, 1.904 coincidem com `IP.BIN+0x1000..0x177F`; 16
  diferem nos offsets relativos
  `0x18D`, `0x18F`, `0x191–0x193`, `0x196–0x197`, `0x199–0x19B`, `0x19D`,
  `0x1B0–0x1B1`, `0x1B3`, `0x1BD` e `0x1DC`. O snapshot RAM do mesmo quadro contém em
  `0x060507C0` o descritor `{count=0x780, destination=0x25C60000,
  source=0x86003000}`. O consumidor em `0x06012D3C` percorre descritores de 12
  bytes; o construtor em `0x0601B0A0` calcula a origem como
  `0x06003000 + [0x06068598]`, e a rotina em `0x06012E60` grava o descritor.
  `0x06010340` define `R14=0` e um caminho por `0x060103A8` pode gravá-lo no
  global; outro desvio pula essa gravação. O snapshot só prova que o campo
  terminou zero. Portanto, a causa imediata do glitch é o DMA selecionar bytes
  que são quase todos do código de boot como textura.

- O callback de captura agora também salva `frame_######_vdp1-command-list.txt`
  e `frame_######_vdp1-vram.bin` no instante do quadro. Isso evita cruzar o
  framebuffer exibido com a lista/VRAM final de outro banco ou de outro momento.
  A alteração compilou em Release, mas ainda não foi validada em uma nova
  reprodução da cena do jogador.

- Para a próxima captura, o runtime também registra as escritas da CPU/SCU DMA
  na origem suspeita `0x06003000–0x06003780` em
  `sprite-source-buffer-writes.csv` e as transferências SCU DMA que escrevem
  nessa faixa em `scu-dma-sprite-source-transfers.txt`. Os registros mantêm
  passo, bloco/caller e bytes anteriores/novos, incluindo escritas em
  `[0x06068598]`, para mostrar se e quando o código de boot é substituído pelos
  dados de sprite e se a gravação do offset ocorreu.

- A inspeção da imagem do disco confirmou que `PL00.ARC` já está carregado no
  quadro 104998. Blocos completos dele aparecem na captura: tipo 2 em RAM
  `0x060BB040`, tipo 17 na VRAM VDP1 `0x8000`, tipo 11 na VRAM VDP2 `0x50000`,
  tipo 27 na VDP1 `0x78000` e tipo 21 na RAM do SCSP `0x2A2EC`; os tipos 26,
  13 e 33 também aparecem em RAM baixa. Quinze comandos VDP1 (179–194) leem
  dentro do bloco tipo 17. Os cinco comandos de X, porém, leem `0x60080–0x60280`
  a partir da faixa `0x06003000`. Isso elimina a hipótese de que o pacote
  `PL00.ARC` inteiro não foi lido; a falha está na seleção ou cópia do conjunto
  separado de tiles do jogador para `0x60000`. Os arquivos extraídos estão em
  `build/rockman/`; a tabela de offsets e tamanhos está em
  `docs/ROCKMAN-X4-STATIC-RECOMPILATION.md`.

- A continuação cruzou os outros recursos com o mesmo snapshot. O payload tipo
  17 de `PL02.ARC` (`0x39D40`, `0x1B800` bytes) coincide integralmente com a
  VRAM VDP1 `0x8000–0x237FF` e com o payload correspondente de `PL00.ARC`.
  O tipo 27 de `PL00.ARC` em `0x5812C` é a interface de seleção, com retrato do
  X e nomes das fases; não identifiquei ali uma sequência de animação de
  gameplay. Também confirmei que `ST0_0_0.ARC[0x4DE00..0x82500]` coincide byte
  por byte com VDP1 `0x24074..0x58774` (`0x34700` bytes), uma faixa grande de
  gráficos da fase que é distinta da origem usada pelos cinco comandos de X.
- A extração foi ampliada para os 76 arquivos `.ARC` da ISO. Dois pacotes
  posteriores da fase inicial contêm arte claro do personagem: tipo 14 de
  `ST0_1_0.ARC`, em `0x4C698` (`0x17000` bytes), e tipo 35 de `ST0_1_1.ARC`,
  em `0x800` (`0x2E700` bytes). As folhas de contato em
  `build/rockman/contacts/ST0_1_0_type14.png` e
  `build/rockman/contacts/ST0_1_1_type35.png` mostram tiles do X. O tipo 35
  contém 92 unidades completas de `0x800` bytes e uma cauda de `0x700`; o DMA
  do sprite copia `0x780` bytes (15 tiles de 128 bytes), uma relação possível
  entre unidade de arte e buffer, ainda sem mapeamento confirmado pelo loader.
- A busca por tiles exatos de 128 bytes encontrou 252 tiles não nulos de
  `ST0_1_0` e 157 de `ST0_1_1` em algum ponto da VRAM VDP1, porém nenhum no slot
  `0x60000–0x607FF`. Os matches ficam em outras regiões VDP1; não houve matches
  não triviais em RAM alta/baixa ou SCSP. Então a arte existe no disco e parte
  de tiles relacionados está residente, mas os cinco comandos que falham ainda
  leem bytes do IP.BIN. Isso não prova que os dois pacotes nunca foram lidos,
  pois o jogo pode transformar os dados ou usar outro buffer.
- As extensões ISO são contíguas: `ST0_0_0.ARC` começa em LBA 1492/FAD 1642,
  `ST0_1_0.ARC` em LBA 1825/FAD 1975 e `ST0_1_1.ARC` em LBA 2099/FAD 2249.
  O histórico de CD PLAY da iteração 41 registra 333 setores a partir de FAD
  1642, exatamente o tamanho do pacote base; a janela final de 16 PLAYs não
  mostra os dois FADs seguintes. Como a janela é truncada, isso ainda é uma
  pista, não prova de que os overlays foram ignorados. A sequência
  `0xC010–0xC050` não aparece nos 76 `.ARC` nem nos quatro `.BIN` extraídos
  `ST00_0/1_X/Z`.
- A origem exata da animação e a rotina que a copia continuam em aberto. A
  correspondência entre unidade `0x800` do tipo 35 e DMA `0x780` é uma hipótese
  para guiar a próxima captura, que deve registrar leitura de FAD 1975/2249 e
  escritas em `0x06003000–0x06003780` na mesma execução.
- A leitura dos blocos AOT esclareceu o zero em `[0x06068598]`: o caminho em
  `0x060103A8` carrega o ponteiro do literal `0x06010464` (valor capturado
  `0x06068598`) e grava `R14`; o bloco de inicialização anterior define
  `R14=0`. O construtor usa o mesmo global pelo literal `0x0601B160`. Portanto,
  zero pode ser o valor inicial esperado; ainda falta localizar quem deveria
  preencher a faixa de origem `0x06003000`.
- Nesta sessão, o provedor REA rejeitou a imagem MDF e o `0.BIN` SH-2 bruto por
  formato não suportado. As novas conclusões vieram da extração local e da
  comparação byte a byte com os snapshots da cena.

- O runtime foi ajustado para reter os 8.192 DMA SCU mais recentes. O arquivo
  `scu-dma-transfers.txt` agora inclui passo, bloco e caller SH-2 para cada
  transferência. A iteração 47 terminou em 510.000.000 blocos e repetiu a
  divergência: captura só com o céu, nenhum sprite VDP1 e FAD 65241. O histórico
  tardio ficou dominado por cópias de 832 bytes de `0x06060594` para
  `0x25C00000`, bloco `0x06034B5C`, caller `0x0F000018`.
- Para preservar uploads mesmo quando o histórico geral é dominado por cópias
  do command list, o runtime agora também grava
  `scu-dma-vdp1-texture-transfers.txt`, filtrando o intervalo de textura
  `0x05C60000–0x05C7FFFF` e o alias `0x25Cxxxxx`. A nova versão compilou em
  Release; ainda falta uma reprodução que alcance a cena corrompida para
  validar o arquivo seletivo.

## Como continuar

1. Recuperar a rota exata da iteração 41 e registrar eventos do pad/SMPC junto
   aos estados de CD, identificando a transição que solicita `ST00_0_X.BIN`
   em FAD 12965 em vez de `ST00_0_Z.BIN` em FAD 18061. A rota repetida nas
   iterações 46–49 termina na cena do céu e não solicita os FADs 1975/2249.
2. A partir de uma rota que mostre X, verificar quem deveria preencher
   `0x06003000–0x06003780`, de qual arquivo/offset e em qual destino. Só então
   mapear o tipo 35 de `ST0_1_1.ARC` (unidades de `0x800` bytes) ao upload de
   `0x780` bytes observado.
3. Corrigir o caminho de carregamento/seleção com essa evidência, repetir uma
   captura curta da mesma cena e conferir os bytes de textura antes de ampliar
   a cobertura a outras fases e overlays.

Cada execução longa leva dezenas de minutos. Reaproveitar a sequência e o ponto
de captura desta iteração para evitar varrer novamente a abertura sem
necessidade. `--vblank-blocks 25000` acelera a agenda do emulador e serve para
diagnóstico; não representa FPS de jogo em tempo real.

## Build

O alvo `saturn-sh2` em Release foi compilado com:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build --config Release --target saturn-sh2
```

Não foi executada a suíte automatizada neste checkpoint. O próximo critério de
validação é demonstrar, em capturas comparáveis da fase, que X se desloca em
resposta ao controle; depois disso, validar a correção visual do sprite.

## Iteração 48 — trilha completa de setores CD

A repetição da sequência da iteração 41 chegou ao limite de 510 milhões de
blocos, mas divergiu para a cena de céu: quadro
`build/artifacts/player-test-iter48-cd-sector-trace/captured-frame.png`, lista
VDP1 com 25 comandos e zero sprites; ambos os bancos VDP1 estavam vazios. Isso
não reproduz o quadro corrompido da iteração 41.

A nova trilha registra 41 comandos PLAY e 37.668 setores efetivamente
percorridos, sem registros descartados. A única leitura do recurso da fase foi
FAD 1642–1974 (333 setores), que corresponde exatamente a `ST0_0_0.ARC`; o PLAY
17 terminou em 1975, exclusivo. Não houve leitura dos FADs 1975 ou 2249, início
dos pacotes `ST0_1_0.ARC` e `ST0_1_1.ARC`. O arquivo seletivo de transferências
SCU para `0x06003000–0x06003780` também ficou vazio. Portanto, esta reprodução
não percorreu a carga dos overlays nem chegou ao upload dos tiles do jogador.

O probe de RAM observou 361.910 stores, mas reteve apenas os 8.192 mais recentes;
todos eram regravações do offset zero em `[0x06068598]`. O buffer circular perdeu
as linhas antigas que poderiam conter writes de sprite. O probe foi ajustado
para contar stores do offset, registrar só mudanças de valor e priorizar os
writes reais em `0x06003000–0x06003780`. O build Release compilou após essa
alteração; ainda falta uma captura em uma rota que mostre X.

Os arquivos `cd-play-trace.csv` e `cd-sector-trace.csv` dão a lista completa de
PLAYs e setores desta execução. O `run.txt` com `--trace` ficou muito grande
(562 MB); nas próximas capturas, usar apenas `--trace-cd-sectors` e o probe
seletivo de sprite. Antes de outra repetição longa, recuperar a diferença
de entrada/estado que fez a iteração 41 chegar à fase, pois as iterações 46–49
terminaram na tela de céu apesar da mesma sequência programada.

## Iteração 49 — confirmação do caminho divergente e buffer de textura

Repeti a sequência documentada da iteração 41 até 510.000.000 blocos, com
`--vblank-blocks 25000`, `--trace-cd-sectors`, um único quadro no bloco
490.000.000 e sem `--trace` geral. A captura `build/artifacts/player-stage-confirm-20261012/captured-frame.png`
mostra apenas céu. O quadro sincronizado é o 360703; os dois bancos/framebuffer
VDP1 estão vazios e o estado final tem 25 comandos, zero sprites. Isso coincide
com a rota divergente da iteração 48 e não reproduz o quadro 104998 da iteração
41, que tinha 60 comandos e 35 sprites.

O trace completo contém 41 PLAYs e 37.668 setores, sem registros descartados.
O PLAY 17 solicita FAD 1642 até 1975 (fim exclusivo), exatamente 333 setores
de `ST0_0_0.ARC`. Não há registros de setor nos FADs 1975 ou 2249 e nenhum PLAY
começa nesses FADs: nesta rota, o jogo não solicita `ST0_1_0.ARC` nem
`ST0_1_1.ARC`. Não é uma falha de retenção no meio do PLAY. O PLAY 18 solicita
FAD 18061 por 3.988 setores (`ST00_0_Z.BIN`). O PLAY 38 solicita FAD 42397 por
5.004 setores: os extents contíguos são `ST04_0.BIN` (LBA 42247, 192 setores)
e `ST04_1.BIN` (LBA 42439, 4.812 setores). Ambos já estão extraídos em
`build/rockman/`. Portanto, a relação dos FADs 1975/2249 com o glitch continua
hipótese; a repetição confirma que esta rota escolhe outros recursos.

O FAD final 12965 da iteração 41 mapeia para o início de `ST00_0_X.BIN`
(LBA 12815, 1.284 setores), enquanto o PLAY 18 desta repetição começa no
`ST00_0_Z.BIN` (FAD 18061, LBA 17911). Isso sugere que a divergência também
passa pela seleção do recurso X/Zero. Ainda não há registro do evento SMPC ou
da leitura do estado do jogo que explique por que a mesma sequência programada
chegou a esses arquivos diferentes.

Esse contraste também aparece antes: a iteração 32 inicia em FAD 18061 e termina
com 25 comandos VDP1 e zero sprites, como as iterações 48–49; a iteração 41
inicia seu stream final em FAD 12965 e mostra 35 sprites no quadro capturado.
Isso reforça a divergência entre as rotas X e Zero, mas não prova que a seleção
de personagem seja a causa do conteúdo incorreto na rota X.

O probe registrou 72 escritas de CPU na faixa de origem
`0x06003000–0x0600377F`, todas na inicialização, a última no bloco 123.325; não
houve DMA SCU escrevendo nessa faixa. Em contraste, houve 2.643 DMA de RAM para
textura VDP1, com origem `0x06003000`, destino pelo alias `0x25C60000`, chamados
em `0x06012DBA` (bloco `0x06012DBC`). No snapshot do quadro, os bytes de RAM
`0x06003000–0x0600377F` diferem em apenas 16 posições de `IP.BIN+0x1000`, e a
VRAM VDP1 `0x60000–0x6077F` coincide byte a byte com essa RAM. O global
`[0x06068598]` está zero no snapshot. Isso confirma uma origem de textura
inadequada nesta rota, mas como o quadro não contém sprites de jogador não
prova que essa seja a causa do glitch de X.

Separadamente, o trace registra 145 transferências SCU de 2 KiB do buffer do
CD (`0x25818000`) para a VRAM VDP1 (`0x05C7xxxx`), entre os blocos
12.320.904 e 388.321.045. Amostras dessas regiões não coincidem byte a byte
com os arquivos ST04 extraídos; a relação entre offsets dos setores e esses
uploads ainda precisa ser determinada.

Os artefatos sincronizados estão em
`build/artifacts/player-stage-confirm-20261012/`: `cd-play-trace.csv`,
`cd-sector-trace.csv`, `sprite-source-buffer-writes.csv`,
`scu-dma-sprite-source-transfers.txt`,
`scu-dma-vdp1-texture-transfers.txt` e `video-diagnostics/frame_360703*`.
O diretório ocupa 12.418.749 bytes. Nenhum artefato anterior foi removido.

## Comparações de resolvedor e AOT (2026-10-12)

O experimento temporário com a lógica antiga de `RuntimeBlockResolver` não
chegou ao quadro agendado. Ele foi interrompido após 30 minutos no passo
146.000.000, no bloco `0x0603084C`, antes da primeira entrada de controle em
380.000.000. O log `build/artifacts/player-stage-legacy-resolver-20261012/run.txt`
ocupa 49.749 bytes. Como não percorreu a divergência X/Zero,
esse teste não confirma nem descarta o resolvedor como causa da rota.

Depois de restaurar `libs/sh2/src/runtime.cpp` ao conteúdo original e reativar
`SATURNBOX_STATIC_MODULE_SOURCE` para
`build/rockman/rockman_player_stage_observed.cpp`, compilou-se Release. A
comparação AOT foi limitada a 510.000.000 passos, quadro único no passo
490.000.000, `--trace-cd-sectors` sem trace geral, 45 minutos e orçamento de
100 MiB. O processo foi encerrado pelo limite de tempo sem gravar o quadro nem
o resumo final; `run.txt` contém apenas `VDP1 renderer: CPU reference`, e o
diretório `build/artifacts/player-stage-aot-route-20261012/` soma 32 bytes. Não
há dado suficiente para afirmar em que passo parou ou qual rota selecionou.

Os resumos já existentes de 41 e 48 mostram o mesmo agendamento SCU
(`period=25000`, `vblank=20400`, `vblank-out=20399`, `dropped=0`), então a
diferença não é explicada por outro período de VBlank nem por perda de registros
de setor. Os contadores de comando CD, porém, divergem (`0x10`: 99 vs. 41;
`0x48`: 81 vs. 1171; `0x51`: 109143 vs. 75175), confirmando fluxos de controle
do jogo diferentes. O estado SMPC amostrado pelo jogo continua sem registro.

Na iteração 41, o resumo final registra PLAY em FAD 12965, filtro com faixa de
1.284 setores e 5.096 setores pedidos/retidos; a última transferência CD foi no
LBA 13241, 426 setores após o início de `ST00_0_X.BIN` (LBA 12815). Isso
comprova atividade no stream de X, mas, sem trace por setor nessa iteração, não
estabelece o percurso completo dos dados até RAM nem o destino final.

Entre o código-base associado à captura 41 e o build atual, o resolvedor SH-2
passou a comparar snapshots/fingerprints do código vivo em endereços já
resolvidos, para invalidar blocos quando overlays alteram instruções. Essa é
uma mudança semântica plausível para testar contra a divergência. Já as chamadas
novas de trace CD são condicionadas ao flag de captura e não alteram o estado do
CD. A tentativa com o resolvedor antigo terminou antes do primeiro input em
380.000.000, portanto a hipótese do resolvedor permanece sem teste conclusivo.

Esses dois testes são inconclusivos e não sustentam correção no carregador. O
último defeito diretamente comprovado continua sendo o quadro da iteração 41:
os cinco comandos de X leem tiles de `0x06003000`, cujo conteúdo é quase todo
`IP.BIN`, mas a rotina que deveria preencher essa origem não foi observada. A
correção visual e a relação com FADs 1975/2249 continuam abertas. O resolvedor
atual e a configuração AOT permanecem restaurados; nenhum artefato anterior foi
removido.

## Instrumentação seletiva de controle (2026-10-12)

Foi adicionado `--control-trace <arquivo.csv>` ao `boot`/`launch`, sem ativar o
trace MMIO geral nem capturar quadros. O CSV registra a aplicação de cada
entrada programada, leituras e escritas SMPC e o pacote CD completo no commit de
CR4, sempre com passo, bloco e chamador. O comando CD de status `0x00` é omitido
porque é polling de alto volume. `--control-trace-from-step N` limita o começo
da janela; o buffer retém no máximo 262.144 eventos e informa quantos foram
descartados.

O smoke test com disco real percorreu 1.000.000 de passos, sem gravar quadros.
Da janela iniciada no passo 200.000, o CSV reteve 1.730 eventos: 226 leituras
SMPC, 89 escritas SMPC e 1.415 comandos CD, sem descarte. O arquivo
`build/tmp/control-trace-boot-smoke-20261012.csv` ocupa 139.553 bytes. Com
`--pad1 start`, o jogo pediu `INTBACK` em 350.029; depois leu `OREG0=0xF1`,
`OREG1=0x02`, `OREG2=0xF7` no passo 547.188, `OREG3=0xFF` e `OREG4=0xF0`.
`OREG2=0xF7` é o bit de Start ativo no pacote digital, então a entrada chega ao
leitor do jogo. Esse teste confirma a instrumentação e o protocolo básico; não
alcança a decisão de personagem nem verifica o glitch visual.

Para preservar a borda da rota sem registrar o polling inteiro, foi adicionado
`--control-trace-oreg2-only`: mantém mudanças de pad, leituras de OREG2 e
comandos CD, descartando as demais leituras/escritas SMPC. A reprodução completa
até 510 milhões e seus resultados estão registrados abaixo.

```powershell
& build/tools/saturn-sh2/Release/saturn-sh2.exe boot build/rockman/aip.bin `
  --aip-base 0x06002E40 --ip build/rockman/ip.bin --ip-base 0x06002000 `
  --program build/rockman/0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf' --disc-sector-size 2448 `
  --continue-after-execdmp --vblank-blocks 25000 --steps 510000000 `
  --pad-sequence '380000000:start;385000000:right;400000000:a;420000000:right;432000000:a;450000000:right;470000000:right,b;495000000:release' `
  --control-trace-from-step 375000000 --control-trace-oreg2-only `
  --control-trace build/tmp/control-trace-route-filtered-20261012.csv `
  --frame-dir build/artifacts/player-stage-control-filtered-20261012 `
  --frame-capture-after-blocks 505000000
```

## Prefixo da rota até 420 milhões (2026-10-12)

O replay foi limitado ao passo 420.000.000, sem `--frame-dir`, e registrou
eventos a partir de 375.000.000. O CSV tem 19.763.403 bytes, 262.144 eventos
retidos e 49.991 descartados do começo da janela; as três mudanças de controle
continuam presentes. O primeiro evento retido é do passo 377.184.454. O arquivo
é `build/tmp/control-trace-route-prefix-20261012.csv`.

O jogo consumiu cada estado programado em OREG2: Start (`0x10`) virou `0xF7` no
passo 380.001.138; direita (`0x01`) virou `0x7F` no passo 385.007.737; A
(`0x20`) virou `0xFB` no passo 400.007.113. Portanto a diferença entre rotas
não vem de essas três entradas serem ignoradas nesta reprodução. Os valores
SMPC das iterações antigas não foram registrados, então isso ainda não explica
sozinho a divergência entre elas.

O primeiro PLAY após Start foi no passo 380.008.582, FAD 654 por 46 setores.
Também aparecem FAD 11.923 no passo 380.556.509, FAD 48.713 no passo
381.854.492 e FAD 42.397 por 5.004 setores no passo 393.804.874, correspondente
ao extent `ST04_0.BIN` seguido por `ST04_1.BIN`. Até o último evento retido
(passo 419.994.221), ainda não houve PLAY em FAD 1642; a sequência programada
para o passo 420.000.000 não chegou a ser aplicada porque a execução parou no
limite desse passo. Assim, esta janela explica os primeiros PLAYs da rota, mas
ainda não explica por que o trace completo da iteração 48 termina sem os FADs
1975 e 2249.

## Replay filtrado completo até 510 milhões (2026-10-12)

O filtro `--control-trace-oreg2-only` reteve 194.535 eventos sem descarte em
14.800.626 bytes: 155.518 leituras OREG2, 39.009 pacotes CD e as oito mudanças
de controle. A reprodução percorreu 510.000.000 passos e capturou um único
quadro no passo 505.000.000. O resumo está em
`build/tmp/control-trace-route-filtered-20261012.txt`; o CSV está no caminho
acima, e os diagnósticos do quadro ocupam
`build/artifacts/player-stage-control-filtered-20261012/` (11.140.811 bytes).

Todos os estados programados chegaram ao leitor do jogo: Start `0x10` → OREG2
`0xF7` no passo 380.001.138; direita `0x01` → `0x7F` em 385.007.737; A
`0x20` → `0xFB` em 400.007.113; direita → `0x7F` em 420.026.323; A → `0xFB`
em 432.036.168; direita → `0x7F` em 450.023.747; direita+B `0x81` → `0x7E`
em 470.036.734; e soltura → `0xFF` em 495.023.726. Portanto, o replay não
perdeu a entrada na interface host nem no leitor SMPC.

Mesmo assim, no intervalo registrado de 375–510 milhões não houve PLAY em FAD
1642, 1975 ou 2249. Os dez PLAYs observados foram FAD 654, 11.923, 48.713,
749, 1.051, 4.378, 42.397, 700, 65.240 e 65.241; o último solicita 10.261
setores. O fluxo não reproduz a leitura de `ST0_0_0.ARC` documentada na
iteração 48. Os resumos também divergem nos comandos `0x48` (1.080 contra
1.171) e `0x51` (73.906 contra 75.175), embora os dois registrem 41 PLAYs
totais. A iteração 48 não guardou OREG, então ainda não é possível atribuir a
diferença a um botão específico.

O quadro 505.000.000 (sequência 329.764) mostra uma tela de arte, sem gameplay:
25 comandos VDP1, zero sprites, bancos/framebuffer VDP1 vazios. O probe seletivo
registrou 72 stores de CPU na faixa `0x06003000–0x0600377F`, todos até o passo
123.325, e nenhum DMA SCU escrevendo nessa faixa. Ainda assim, o trace contém
2.643 DMA de RAM para textura VDP1, origem `0x06003000`, destino pelo alias
`0x25C60000`, no bloco `0x06012DBC` / caller `0x06012DBA`. O DMA de 1.920 bytes
no passo 418.016.701 e as cópias menores dos passos 418.024.299 e 418.042.515
usam `from_cd=no`.

No snapshot, 1.904 dos 1.920 bytes de
`0x06003000–0x0600377F` coincidem com `IP.BIN+0x1000`; o primeiro byte diferente
fica no deslocamento `0x18D`. Os 1.920 bytes também coincidem integralmente
com a RAM da iteração 41. A VRAM VDP1 em `0x60000–0x6077F` é cópia byte a byte
da mesma origem. Isso localiza a falha antes do DMA: a rotina transfere os bytes
que recebe, mas o buffer ainda contém essencialmente dados do IP. Esta rota não
tem sprites de jogador, portanto não demonstra sozinha a correção visual. Não
foi alterado o carregador nem a lógica do DMA; falta identificar qual rotina
deveria preencher o buffer a partir do recurso de jogador.

O mesmo trace registra 145 DMA de 2 KiB do buffer CD `0x25818000` para VDP1
`0x05C7xxxx`, entre os passos 12.340.932 e 389.541.042. A origem desses blocos
em arquivo ainda não foi mapeada. O replay e a captura única adicionaram
45.384.735 bytes entre traces, resumos e diagnósticos; nenhum quadro em
sequência foi produzido. O diretório total `build/artifacts/` agora ocupa
29.522.039.135 bytes, incluindo os artefatos preexistentes.

## Comparação JIT e AOT, e resolvedor legado (2026-10-18)

Uma reprodução JIT até 510.000.000 passos, com a mesma sequência de controles
e captura no passo 505.000.000, executou os 510 milhões de blocos pelo JIT. O
trace reteve 218.610 eventos sem descarte. Cada estado scripted foi lido no
OREG2: Start `0xF7` em 380.024.960; direita `0x7F` em 385.001.067; A `0xFB` em
400.008.617; direita `0x7F` em 420.000.603; A `0xFB` em 432.000.831; direita
`0x7F` em 450.000.669; direita+B `0x7E` em 470.000.566; soltura `0xFF` em
495.000.423. Arquivos: `build/tmp/control-trace-jit-full-20261018.txt` e
`build/tmp/control-trace-jit-full-20261018.csv`.

O histórico de CD PLAY coincide com o AOT da iteração 48: 41 PLAYs e os mesmos
FADs, incluindo FAD 42.397 (`ST04_0.BIN`) e FAD 65.241 no fim; não há PLAY em
FAD 1.642, 1.975 ou 2.249. Os totais `0x48`/`0x51` variam entre JIT (1.171 /
75.175) e AOT (1.080 / 73.906). A imagem AOT deste replay é a ilustração da
abertura com X e Zero (sequência 329.764), confirmada pelo usuário; a imagem
JIT mostra outro instante da abertura, com X caindo sobre o céu azul (sequência
374.707). Ambos têm 25 comandos VDP1 e zero sprites, compatíveis com essas
cenas de abertura. A diferença entre os quadros, por si só, não demonstra
defeito visual nem divergência semântica dos backends. Capturas:
`build/artifacts/player-stage-jit-full-20261018/` e
`build/artifacts/player-stage-control-filtered-20261012/`.

Para isolar o resolvedor, uma variante do código atual restaurou apenas o
cache legado por endereço, sem fingerprint do código vivo. Ela também concluiu
510.000.000 passos, mas repetiu os totais AOT (`0x48=1.080`, `0x51=73.906`), o
mesmo histórico de CD e 25 comandos VDP1 sem sprites; o quadro capturado ficou
preto e não serve para classificar a tela do jogo. Portanto, restaurar apenas a
política antiga do resolvedor não recupera a cena da iteração 41. Um build
limpo da revisão `42b913a` também não a recuperou, mas essa execução muda
outros componentes e não é uma comparação isolada do resolvedor. Resumo da
variação controlada:
`C:/Users/rapin/.codex/worktrees/resolver-address-only/saturnbox/build/tmp/current-old-resolver-20261018.txt`.

A causa da divergência entre a iteração 41 e a rota atual permanece aberta.
Como os quadros JIT e AOT acima ainda mostram a abertura, o próximo passo é
localizar a transição da abertura para o menu e confirmar que Start/confirmação
são lidos nesse estado. OREG2 comprova que os valores do controle chegam ao
leitor SMPC, mas não que o fluxo do jogo os consome para avançar a tela. O
histórico em `docs/ROCKMAN-X4-TITLE-MAP-FIX.md` já registra um toque Enter
durante `OP.CPK` que pula para o título e um replay com Start entre 13,7 e 14
milhões (VBlank de 14.000) que alcança recursos da primeira fase; esse replay
fica parado no streaming e ainda não mostra sprites de gameplay. Esses pontos
separam o avanço da abertura do bloqueio posterior de carregamento. Depois de
estabelecer a transição neste build, comparar a rota até a fase com a iteração
41; FAD 42.397 não deve ser tratado como seleção incorreta sem identificar
antes o estado do jogo. Não alterar o carregador ou DMA com a evidência atual.
A REA não aceitou `0.BIN` como formato binário suportado; a inspeção do SH-2
continua limitada aos decodificadores e ferramentas locais do projeto.

## Reentrada após a abertura e teste do direcional (2026-10-18)

Foi repetida a rota curta documentada com VBlank de 14.000 blocos. A imagem no
passo 19.500.000 mostra a tela de título do Rockman X4; o OREG2 leu Start como
`0xF7` a partir do passo 13.702.023. Com A entre 21.000.000 e 21.300.000, o
quadro do passo 25.000.000 mostra `Now Loading...`; o OREG2 leu A (`0xFB`) a
partir do passo 21.288.291. No passo 30.000.000, o CD já avançou para 446/3.988
setores do stream em FAD 18.061 (`ST00_0_Z.BIN`), mas isso ainda é carregamento,
não gameplay. Capturas e traces: `build/artifacts/player-stage-transition-check-20261018/`,
`build/tmp/player-stage-transition-check-20261018.txt` e
`build/tmp/control-trace-transition-20261018.csv`.

Uma continuação aplicou Start em 45.000.000 e A em 48.000.000. O quadro de
68.000.000 mostra a cena da fase, com 217 comandos VDP1 (187 sprites); o stream
de FAD 18.061 chegou a 902/3.988 setores e o buffer reteve 200. OREG2 leu
Start como `0xF7` e A como `0xFB`. O overlay `PRESS START BUTTON` já observado
na captura de 42 milhões da iteração anterior identifica a demonstração da
fase como modo attract, então esta captura sozinha ainda não prova controle
humano.

Para medir o direcional, repeti o mesmo percurso e quadro sem direita entre
60.000.000 e 63.000.000. A execução com direita registrou 421 leituras de
OREG2=`0x7F`; apesar disso, os dois arquivos PPM no passo 68.000.000 são
byte a byte idênticos (SHA-256
`6e837eb7c484aae9daf9577cc3467a27079b96a0a4a15162744b104e552721d1`). Os
dumps de RAM alta/baixa, RAM SCSP, VRAM/CRAM VDP2, VRAM e command list VDP1
também são idênticos. Os resumos de CD coincidem, incluindo 902/3.988 setores,
200 retidos e 217 comandos VDP1. Portanto, a entrada chega ao leitor SMPC, mas
não altera o estado em RAM nem a saída de vídeo nesta rota. Como o estado pode
continuar sendo demonstração automática, isso não basta para concluir que o
direcional do jogador está quebrado.

Naquele replay de 68 milhões, a tela `PRESS START BUTTON` identificava uma
demonstração automática; por isso ele não servia para medir controle. A rota
curta descrita abaixo contorna essa captura e seleciona X diretamente. A nova
comparação mostra que a entrada altera o fluxo e a saída, mas ainda falta
separar deslocamento do jogador de animação/transição. O próximo diagnóstico
segue o produtor GFS e a cópia que deveria preencher `0x06003000`; não alterar
renderer ou DMA sem evidência de uma falha nesses dispositivos.

## Rota curta até a fase X e origem do sprite (2026-10-18)

A rota curta com VBlank de 14.000 blocos agora reproduz a seleção de X sem
percorrer novamente o replay longo da iteração 41. Start em 13.700.000 abre o
título; Start em 20.000.000 abre o menu; A em 24.000.000 seleciona GAME START;
A em 28.000.000 confirma X; Start em 46.000.000 pula o filme de abertura; A em
61.000.000, 82.000.000, 94.000.000, 102.000.000 e 110.000.000 avança o diálogo
até o carregamento de `ST00_0_X.BIN` em FAD 12.965. O frame de 118.000.000 já
mostra a primeira cena da fase. A captura da abertura confirmada pelo usuário
continua sendo um quadro normal do filme.

Para testar o controle, foram executadas duas rotas idênticas até 110.000.000,
uma sem entrada posterior e outra com direita entre 122.000.000 e 145.000.000.
O trace OREG2 da rota com direita leu `0x7F` 2.842 vezes nesse intervalo. No
mesmo passo 155.000.000, os PPM diferem em 72.133 de 76.800 pixels. A rota com
direita também emite um CD PLAY a partir de FAD 14.249, início de
`ST00_1_X.BIN`, na partição 1; até o fim do replay em 160.000.000, 354 setores
foram consumidos. Esse comando começa no passo 157.531.564, depois do quadro de
155.000.000, então não explica a diferença entre os PPM daquele passo. A rota
neutra não inicia esse PLAY durante o trecho capturado. Os comandos VDP1 que leem a origem
`0x60000`/`0x60080` mudam de coordenadas entre as capturas. Isso confirma que a
entrada altera o fluxo e a saída do jogo, embora ainda não prove o deslocamento
contínuo do personagem em uma janela de gameplay estável. Capturas e traces:
`build/artifacts/player-stage-x-right-20261018/`,
`build/artifacts/player-stage-x-neutral-20261018/`,
`build/tmp/control-trace-x-right-20261018.csv`.

O frame sincronizado da rota com direita fecha a cadeia do defeito visual:
2.032 dos 2.048 bytes em RAM `0x06003000–0x060037FF` coincidem com
`IP.BIN+0x1000`; o restante difere em 16 bytes. A faixa VDP1
`0x60000–0x607FF` coincide byte a byte com essa RAM. Os comandos VDP1 usam
essa textura e o SCU DMA copia a partir de `0x06003000` para o alias VDP1
`0x25C60000`. O global `[0x06068598]` segue zero, não houve escritas de CPU no
buffer de tiles depois da inicialização e o arquivo seletivo de DMA para a
origem permanece vazio. Portanto, o DMA entrega fielmente o conteúdo que o
jogo selecionou; a falha observada está antes da cópia, no preenchimento ou na
seleção do buffer. Nenhuma correção no renderer ou no DMA é sustentada por esta
evidência.

O histórico CD dessa comparação não solicita FAD 1.975 ou 2.249, início dos
overlays `ST0_1_0.ARC` e `ST0_1_1.ARC`. A relação desses overlays com os tiles
continua em aberto. Próxima prioridade: seguir a rotina que carrega o stream
FAD 14.249 e identificar qual leitura/cópia deveria substituir os bytes em
`0x06003000`; depois validar movimento com quadros da mesma cena estável. Isso
segue o critério do [workplan de SaturnRecomp](https://github.com/sonsegajp/SaturnRecomp/blob/main/SATURNRECOMP_WORKPLAN.md): demonstrar compatibilidade por boot, transições, entrada e gameplay reproduzível, e expressar correções como comportamento geral de hardware/runtime.

## Separação entre FMV e sprite de gameplay (2026-10-18)

A imagem anexada pelo usuário é da abertura. O trace GFS da rota confirma um
fluxo de vídeo separado: registrou 104 limites de decodificação no buffer
`0x060B49CC`, cobrindo as sequências 153–857 até o passo 7.047.740. A rotina
limpa esse buffer mais tarde, no passo 102.180.859. Esse destino não é a origem
`0x06003000` usada pelos tiles de X, portanto o trace da abertura não localiza
quem deveria preparar o sprite da fase.

Uma repetição com liberações explícitas entre os toques chegou à cena de X e
capturou o quadro de sequência 39.947 no passo 160.000.000. O CD iniciou
`ST00_0_X.BIN` em FAD 12.965 no passo 140.717.115 e repetiu o PLAY em
155.697.186; o trace de setores chegou a FAD 13.440, ainda antes do início de
`ST00_1_X.BIN` em FAD 14.249. Mesmo assim, a origem de textura em RAM
`0x06003000–0x060037FF` continuava com 2.032/2.048 bytes iguais a
`IP.BIN+0x1000`, e VDP1 `0x60000–0x607FF` era uma cópia exata. O probe de
stores monitora a faixa usada no upload, `0x06003000–0x0600377F`, e reteve 76
gravações, todas até o passo 123.333. Confirmei que ele observa gravações via
`MachineState::write*`: isso inclui stores da CPU e as transferências de DMA
SH-2, SCU e CD que usam essa interface. O trace específico de SCU DMA também
não registra transferências escrevendo na faixa. Artefatos:
`build/artifacts/player-stage-x-right-after-stream-tapped-20261018/`.

Na execução direita anterior, o jogo também emitiu CD PLAY a partir de FAD
14.249 no passo 157.531.564 e consumiu 354 setores até o limite 160.000.000.
Esse comando veio depois do quadro de 155.000.000. O trace completo dessa
execução continuou sem gravações via `MachineState::write*` na faixa monitorada
`0x06003000–0x0600377F`, então a passagem por `ST00_1_X.BIN` também não chegou
a preencher o buffer de sprites. Como as rotinas de DMA SH-2, SCU e CD desta
implementação escrevem por `MachineState`, esses caminhos também estão cobertos
pelo probe; não se trata apenas de uma ausência de stores de CPU.

Para avançar a leitura, repeti a rota até 230.000.000 e capturei o quadro de
sequência 61.458 no passo 225.000.000. O PLAY em FAD 12.965 cobre 5.096 setores:
1.284 de `ST00_0_X.BIN` e 3.812 de `ST00_1_X.BIN`. No corte de 230M, o cursor
CD chegou a 4.014/5.096 setores, até FAD 16.978 — 2.729 setores dentro do
segundo arquivo, com 1.082 ainda por ler. Mesmo após essa parte substancial do
stream, o probe continua em 76 stores na faixa `0x06003000–0x0600377F`, todos
até o passo 123.333; 2.032/2.048 bytes da região ampla de 0x800 bytes seguem
iguais a `IP.BIN+0x1000`, e a VRAM VDP1 é cópia exata. O stream ainda está
incompleto, então a ausência do sprite neste corte não basta para concluir que
o carregador falhou. Captura e traces:
`build/artifacts/player-stage-x-right-stream-20261018/` e
`build/tmp/player-stage-x-right-stream-20261018.txt`.

Próxima prioridade: mapear o consumidor das leituras CD da partição 1 até o
buffer de RAM que recebe os setores. O trace detalhado `--trace-game` não liga
esse PLAY ao consumidor GFS do filme: os últimos registros do parser/tarefa,
decodificador, limites de quadro, consumidor do ring e produtor/atualização GFS
ocorreram, respectivamente, nos passos 3.426.757, 6.429.128, 7.047.740,
41.965.961 e 46.015.651; o PLAY de FAD 14.249 começa no passo 251.093.174.
Portanto, não presumir que o caminho de FMV prepare `0x06003000`.

## Leitura completa do primeiro stream X (2026-10-18)

Uma nova repetição até 260.000.000 confirmou que o PLAY de FAD 12.965 percorre
o intervalo inteiro até FAD 18.060, os 5.096 setores contíguos que abrangem
`ST00_0_X.BIN` e `ST00_1_X.BIN`. Depois desse intervalo, o jogo inicia um PLAY
separado de FAD 14.249, com 3.812 setores de `ST00_1_X.BIN` na partição 1. No
limite de 260M, esse segundo PLAY havia avançado 656 setores (cursor 655), ainda
em andamento.

O frame de sequência 71.474 foi capturado em 255.000.000. Até o fim da execução,
o probe ainda registrou somente as 76 gravações de inicialização no intervalo
`0x06003000–0x0600377F`, a última no passo 123.333. Na captura, 2.032/2.048
bytes da faixa ampla `0x06003000–0x060037FF` continuam iguais a `IP.BIN+0x1000`,
e a VRAM VDP1 `0x60000–0x607FF` continua idêntica à RAM. Assim, o primeiro
stream X completo não alterou a origem de sprite; o PLAY separado de
`ST00_1_X.BIN` ainda não terminou, portanto o carregamento posterior desse
arquivo segue como hipótese. Captura e traces:
`build/artifacts/player-stage-x-right-stream-complete-20261018/` e
`build/tmp/player-stage-x-right-stream-complete-20261018.txt`.

No mesmo trace, o SCU repete transferências de `0x06003000` para o alias VDP1
`0x25C60000` a partir do passo 251.025.925. Elas têm `from_cd=0` e copiam a
origem já existente em RAM; não identificam o destino das leituras CD da
partição 1. A captura da abertura é conteúdo normal do filme e fica fora deste
diagnóstico de gameplay.

Próximo passo: rastrear as leituras da FIFO CD (`0x25818000` e o alias de
palavra `0x25898000`) durante o PLAY de FAD 14.249 e correlacioná-las com as
gravações seguintes em RAM. Completar os 3.812 setores também confirma se o
comando termina, mas não substitui localizar o consumidor e o destino. Não há
base para alterar GFS, VDP1 ou DMA neste ponto.

## Trace leve da FIFO CD (2026-10-18)

O probe leve de `--trace-cd-sectors` foi ampliado para gravar
`cd-fifo-read-trace.csv`, sem ativar o trace geral do jogo. Na repetição curta
até 253.000.000 passos, ele registrou 2.718 leituras a partir do passo 200M.
Todas as amostras vieram de leituras de 32 bits em `0x25818000`, no bloco
`0x060152DC`, com destinos em `0x25A7A000–0x25A7F800`; nenhuma leitura da FIFO
de palavra `0x25898000` apareceu nessa execução. O endereço de destino cai na
faixa de alias cache-bypass da SCSP, então essas amostras, por si só, não
identificam o carregamento do sprite do jogador nem alterações no buffer de
RAM de gameplay.

Esta repetição divergiu da execução válida documentada acima: encerrou em
253M com o stream anterior em 5.092/5.096 setores e não reproduziu o PLAY de
FAD 14.249 com CR3=`0x80`, CR4=`0x0EE4` (3.812 setores). Em vez disso, emitiu
um PLAY de FAD 14.249 com CR3=`0xFF`, CR4=`0xFFFF` e tamanho solicitado
`0x7FFFFF`, seguido por outro PLAY de FAD 12.965. Portanto, não correlacionar
as amostras deste CSV com o segundo stream válido nem concluir que ele
carregou recursos de gameplay. Artefatos:
`build/artifacts/player-stage-x-right-cd-fifo-lean-20261018/` e
`build/tmp/player-stage-x-right-cd-fifo-lean-20261018.txt`.

A imagem do usuário confirma que o quadro exibido é a abertura cinematográfica
normal; ela não é evidência de falha de sprite nem de gameplay renderizado.
Próxima prioridade: recuperar a sequência/estado exatos que produzem o PLAY
válido de FAD 14.249 (CR3=`0x80`, CR4=`0x0EE4`), comparar esse estado de título
e então repetir o probe FIFO nessa rota, procurando leituras da FIFO de
palavra e gravações subsequentes em RAM. Só depois correlacionar a origem com
o buffer do jogador.

## Repetição com trace de controle (2026-10-18)

A rota curta foi repetida até 253.000.000 com o mesmo agendamento de entradas,
captura em 252M, FIFO trace e `--control-trace-from-step 200000000`. O resultado
CD repetiu a divergência: o PLAY de FAD 14.249 foi enviado com CR3=`0xFF`,
CR4=`0xFFFF` e tamanho solicitado `0x7FFFFF`, seguido por um PLAY de FAD
12.965; no fim, o stream de FAD 12.965 estava em 5.092/5.096 setores. A tabela
`cd-play-trace.csv` tem 31 PLAYs e nenhum PLAY válido de 3.812 setores em FAD
14.249. O trace FIFO repetiu 2.718 leituras de 32 bits em `0x25818000`, com
destinos no alias SCSP `0x25A7xxxx`; não houve leitura de `0x25898000`. A
imagem enviada pelo usuário é a abertura normal e fica fora do diagnóstico do
sprite de gameplay.

O trace de controle reteve 262.144 registros e descartou 226.049; o primeiro
registro retido é do passo 224.542.597. Como o comando divergente já ocorre
perto de 157,5M, essa janela não preservou o início da divergência. Artefatos:
`build/artifacts/player-stage-x-right-cd-fifo-control-20261018/`,
`build/tmp/player-stage-x-right-cd-fifo-control-20261018.txt` e
`build/tmp/control-trace-x-right-cd-fifo-20261018.csv`.

Uma captura seguinte usou apenas OREG2/CD até 160M e reteve 177.945 eventos,
sem descarte. Cada toque programado foi observado no OREG2: Start em 13,7M,
20M e 46M (`0xF7`); A em 24M, 28M, 61M, 82M, 94M, 102M e 110M (`0xFB`); e
direita em 122M (`0x7F`). Os releases voltaram a `0xFF`; o release do A de
102M só foi lido em 104.151.721. De 135M a 145M, OREG2 confirma direita ativa
até o release e depois neutro, igual ao trace da execução que chega ao PLAY
válido. Ainda assim, esta rota enviou o PLAY malformado no passo 157.531.564.

No trace válido, há atualização dos parâmetros de FAD 14.249 em 140.690.359 e
155.666.860, seguida do PLAY `CR3=0x80`, `CR4=0x0EE4` no passo 251.093.174. Na
rota atual, os parâmetros foram atualizados em 140.873.402; não houve a segunda
atualização em 155.666.860, e o jogo enviou CR3=`0xFF`, CR4=`0xFFFF` em
157.531.564. Como o trace da execução válida começa em 135M, não temos o
histórico de OREG2 anterior para comparar. A divergência restante está no
estado/caminho do jogo antes de 155,7M, não em um toque ausente após 135M.
Próximo passo: recuperar ou repetir a execução válida com trace OREG2 desde o
início e confrontar as transições entre 100M e 155,7M; em seguida localizar a
rotina que prepara os parâmetros e o consumidor do stream válido antes de
retomar a análise FIFO até a RAM do jogador. Artefatos da captura filtrada:
`build/tmp/control-trace-x-right-full-0-160-20261018.csv` e
`build/tmp/player-stage-x-right-full-0-160-20261018.txt`.

## Separação do stream SCSP e dos tiles de X (2026-10-18)

Uma comparação byte a byte do snapshot `scsp_ram.bin` da execução válida
confirmou seis blocos de 4 KiB de `ST00_1_X.BIN`, nos offsets de arquivo
`0x5E000–0x63FFF`, reorganizados em `SCSP RAM 0x05A7A000–0x05A7FFFF`. São
24.576 bytes exatos (192 blocos de 128 bytes), encontrados apenas na SCSP RAM;
nenhuma correspondência alinhada de 128 bytes desse intervalo apareceu em
RAM baixa/alta ou VRAM VDP1. Isso confirma que os destinos `0x25A7xxxx` do trace
FIFO pertencem ao caminho SCSP, provavelmente dados de áudio, e não ao buffer
de tiles do jogador `0x06003000`.

A execução válida de 260M também não consumiu setores dos extents de arte da
fase: `ST0_1_0.ARC` (FAD 1975, 274 setores) ou `ST0_1_1.ARC` (FAD 2249, 94
setores). Esses arquivos contêm os payloads com gráficos de X já localizados
estaticamente, enquanto o stream `ST00_1_X.BIN` não preencheu o buffer de
sprites.

## Replay válido até 320M (2026-10-18)

A rota curta reproduziu o PLAY válido em FAD 14.249, CR3=`0x80`, CR4=`0x0EE4`,
no passo 253.109.169. Em 320.000.000, o cursor ainda estava no setor 3.628 de
3.812; o último FAD lido era 17.876. Portanto, essa execução não permite
concluir se o pacote termina ou se a transição seguinte pede os ARCs. O trace
de setores não contém leituras dos extents FAD 1975 (274 setores) nem FAD 2249
(94 setores). Artefatos: `build/artifacts/player-stage-x-right-resource-complete-20261018/`
e `build/tmp/player-stage-x-right-resource-complete-20261018.txt`.

A captura de sequência 91.139 foi armada em 315M. Ela tem 43 comandos VDP1,
incluindo 17 sprites, mas a origem `0x06003000–0x060037FF` ainda coincide em
2.032/2.048 bytes com `IP.BIN+0x1000`, e a VRAM VDP1 `0x60000–0x607FF` coincide
com essa mesma origem. Assim, há desenho de sprites na cena capturada, mas
nenhuma evidência de que os tiles corretos de X tenham sido carregados nesse
buffer. O trace FIFO reteve 6.152 leituras de 32 bits em `0x25818000`, no
bloco `0x060152DC`, com destinos no alias SCSP `0x25A7xxxx`; não houve amostra
da FIFO de palavra `0x25898000`. Isso continua separado do caminho de textura
do jogador.

Próxima prioridade: repetir a mesma rota além dos 3.812 setores e observar o
PLAY e os setores seguintes, mantendo captura de um quadro e trace de controle
a partir do fim do stream. Se o jogo não solicitar os ARCs ao concluir o
stream, avançar a transição de gameplay que deveria agendar esses pacotes.

## Replay pós-stream até 330M (2026-10-18)

O replay estendido chegou a 330.000.000. Para separar os dois PLAYs: o stream
anterior de 5.096 setores iniciado em FAD 12.965 terminou em FAD 18.060,
cursor 5.095, sequência 17.955. O PLAY válido de 3.812 setores em FAD 14.249
começa na sequência 17.956 e termina em FAD 18.060, cursor 3.811, sequência
21.767. O setor seguinte, sequência 21.768, volta a FAD 14.249 com cursor 0.
O trace de controle localiza esse segundo PLAY válido no passo 323.557.173,
bloco `0x0603554C`, caller `0x060354C8`, com CR1=`0x1080`, CR2=`0x37A9`,
CR3=`0x80` e CR4=`0x0EE4` — repetindo os mesmos 3.812 setores. No limite de
330M, a segunda passagem avançou até FAD 14.778, cursor 529. Os extents
`ST0_1_0.ARC` (FAD 1975) e `ST0_1_1.ARC` (FAD 2249) continuam sem leituras.
Portanto, completar o PLAY válido apenas faz o jogo reproduzir esse stream
novamente; ainda não há prova de que o loader de arte tenha sido chamado.

A captura de sequência 95.475 foi armada em 328M. Ela contém 43 comandos VDP1,
incluindo 17 sprites. A faixa RAM `0x06003000–0x060037FF` ainda coincide em
2.032/2.048 bytes com `IP.BIN+0x1000`; os 2.048 bytes correspondentes em VRAM
VDP1 coincidem integralmente com RAM. O trace filtrado reteve 16.031 eventos,
sem descartes; nele, OREG2 só aparece neutro (`0xFF`) durante a janela, sem
novas entradas programadas. Artefatos: `build/artifacts/player-stage-x-right-post-stream-20261018/`,
`build/tmp/player-stage-x-right-post-stream-20261018.txt` e
`build/tmp/control-trace-x-right-post-stream-20261018.csv`.

Próxima prioridade: avançar a transição de gameplay com entradas após essa
captura e procurar o primeiro PLAY de FAD 1975/2249 junto das escritas no
buffer do jogador. O replay atual parou no loop/repetição de FAD 14249; não
justifica uma alteração no CD, DMA ou VDP1.

## Movimento de X dentro da fase (2026-10-18)

Uma nova repetição manteve toda a rota anterior e acrescentou direita de 330M
a 335M, com captura no fim do movimento. O jogo leu direita em OREG2 como
`0x7F` (602 amostras na janela) e a liberação como `0xFF`; os dois eventos de
pad foram registrados sem descarte. A captura de sequência 97.100 mostra a
cena deslocada em relação ao quadro 328M e a lista VDP1 cresceu de 43 para 282
comandos (17 para 255 sprites). Isso confirma resposta à direita dentro da
fase, embora a câmera acompanhe X.

O sprite de X ainda aparece corrompido. A faixa RAM
`0x06003000–0x060037FF` continua igual a `IP.BIN+0x1000` em 2.032/2.048 bytes,
e os 2.048 bytes de VRAM VDP1 correspondentes são idênticos à RAM. O trace CD
termina na segunda passagem de FAD 14.249, cursor 1.043, sem setores de FAD
1975 ou 2249. Assim, o movimento chegou ao jogo, mas não disparou a leitura
dos ARCs nem o preenchimento da origem de textura. Captura:
`build/artifacts/player-stage-x-right-in-stage-20261018/`; resumo e traces em
`build/tmp/player-stage-x-right-in-stage-20261018.txt` e
`build/tmp/control-trace-x-right-in-stage-20261018.csv`.

O trace DMA confirma a etapa seguinte da cadeia: o SCU canal 0 repete cópias de
`0x06003000` para `0x25C60000`, pelo bloco `0x06012DBC`/caller `0x06012DBA`,
com contagens `0x880` e `0x900`, sempre `from_cd=0`. No snapshot, até a maior
faixa transferida (`0x900` bytes) coincide byte por byte com o código do
`IP.BIN`; a VRAM `0x60000–0x608FF` coincide byte por byte com essa RAM. Os
comandos VDP1 de X continuam lendo `0x60000` e `0x60080`. Logo, entrada,
resposta do jogo, DMA e VDP1 estão ativos; a etapa ausente é o carregamento ou
a seleção da arte antes do DMA.

Próxima prioridade: localizar no caminho de inicialização da fase a chamada
GFS que deveria solicitar `ST0_1_0.ARC`/`ST0_1_1.ARC` e copiar a arte de X para
`0x06003000`. A imagem original enviada pelo usuário é a abertura normal; os
quadros em `player-stage-x-right-post-stream` e `player-stage-x-right-in-stage`
são capturas posteriores da fase e mostram o defeito real do sprite.

## Chaves vistas no resolvedor GFS (2026-10-18)

O probe de nomes compilou em Release e a reprodução curta chegou a 156M passos.
Nos blocos `0x0603AD4A`, R5 aponta para as chaves `ST00_0_X.BIN` e
`ST00_1_X.BIN`, repetidas nos passos 104.294.008/285 e 140.869.633/910. R1 é
um ponteiro para a pilha, não um nome C; o probe foi corrigido para registrar
a chave de R5. O resolvedor abriu 25 recursos nessa execução: 25 entradas no
matcher e caminho pronto, sem hits nos caminhos de descritor não pronto ou de
erro de operação.

Essa captura curta divergiu do replay GFS mais longo antes da janela de
155,66M: ela estava no bloco `0x060124B0`, enquanto a execução anterior passou
por `0x0022A4AA` e registrou mais duas comparações. Portanto, esses nomes
confirmam as buscas dos streams `ST00_*_X.BIN`, mas não identificam o loader de
arte da fase nem explicam quem deveria preencher `0x06003000`. Para continuar,
repetir a rota que chega ao quadro da fase com o mesmo conjunto de flags de
captura e registrar os argumentos do comparador na janela de inicialização;
manter o critério de não alterar CD/VDP1 sem evidência de um pedido de ARC.

## Rota estática do pedido de ARC da primeira fase (2026-10-18)

A análise de `X4LOW.X4` a partir de `0x00271952` percorre 30 blocos e 98
instruções, todos levantáveis para SIR. O emissor C++ gerou os 30 blocos sem
falhas. A análise do helper `0x0601CE2A` em `0.BIN` percorre 15 blocos e 92
instruções, também sem falhas de lifting. Um módulo combinado dos dois segmentos
gerou 1.062 blocos e 15.299 instruções SIR, sem blocos ignorados. O módulo de
prova está em `build/tmp/x4low-stage-resource-path.cpp`; ainda não foi ligado ao
runtime porque o roadmap registra como pendente o versionamento de overlays no
mesmo endereço.

O handler do overlay começa em `0x00271952`; o byte de estado fica em
`R14+5`. Quando o estado é 1, o bloco de comparação em `0x0027196A` segue para
`0x002719CE`, que carrega o template `ST0_1_1.ARC` pelo ponteiro em
`0x00271A50` (para a string em `0x002A3EE8`). O bloco que começa em
`0x002719E8` altera os caracteres nas posições +2/+4 usando `R13+12/+13` e
chama `0x0601CE2A` por `JSR` em `0x002719F6`. O delay slot passa o ponteiro do
nome em `R4`. O probe preserva separadamente a entrada no estado 1, o bloco de
chamada, a entrada no helper e correspondências do resolvedor para os dois
ARCs, incluindo nome, estado e índices; isso evita perder esses eventos na
janela curta do trace geral.

A primeira repetição da rota chegou a 340M passos e capturou o quadro 97.100 em
335M. O trace de controle confirmou a entrada direita programada de 330M a 335M,
com OREG2=`0x7F` em 602 amostras e liberação em `0xFF` em 111 amostras. O trace CD reteve
22.811 setores, sem leituras nos FADs 1975–2342, que cobrem os dois arquivos
`ST0_1_0.ARC` e `ST0_1_1.ARC`. O PLAY anterior vai de FAD 1642 até 1975
exclusivo (333 setores), terminando exatamente antes do extent de `ST0_1_0.ARC`.
No quadro, `0x06003000–0x060038FF` coincide com
`IP.BIN+0x1000` em 2.032/2.304 bytes e com VDP1 VRAM `0x60000–0x608FF` em
2.304/2.304 bytes; os comandos do jogador leem `0x60000`, `0x60080`, …,
`0x60300`. Isso confirma o defeito depois do input e antes da origem do DMA.

Essa primeira execução usou checkpoints em endereços de instruções que não eram
inícios de blocos CFG, então a ausência de chamada no log não prova que o
handler do overlay não foi executado. A repetição com os checkpoints alinhados
aos blocos `0x0027196A`, `0x0027197A`, `0x002719CE`, `0x002719E8` e à entrada do
helper `0x0601CE2A` concluiu até 340M; o resultado está na seção seguinte.
Artefatos da primeira captura:
`build/artifacts/player-stage-archive-loader-20261018/`,
`build/tmp/player-stage-archive-loader-probe-20261018.txt` e
`build/tmp/control-trace-x-stage-archive-loader-20261018.csv`.

## Repetição dedicada do handler de ARC (2026-10-18)

A repetição Release concluiu 340M passos com código de saída 0 e capturou o
quadro 97.100 após a entrada direita. O trace dedicado reteve zero eventos nos
blocos `0x0027196A`, `0x0027197A`, `0x002719CE`, `0x002719E8` e no helper
`0x0601CE2A`: esse caminho não foi chamado nessa rota. Mesmo assim, o overlay
`X4LOW.X4` está carregado: os 64 bytes em `0x00271952` coincidem com o arquivo
em `build/rockman/X4LOW.X4`, e a string `ST0_1_1.ARC` está presente na RAM em
`0x002A3EE8`. Portanto, o resultado não é explicado por código ausente ou por
uma falha de leitura do arquivo de overlay; falta localizar a seleção/dispatch
que deveria invocar o handler, ou confirmar qual outro callback abastece a
origem de textura do jogador.

O trace de CD reteve 22.811 setores sem descartes e não registrou FADs
1975–2342, que contêm `ST0_1_0.ARC` e `ST0_1_1.ARC`. A última lista de PLAY
inclui o intervalo 1642–1975 (333 setores) e a repetição de 14.249–18.061
(3.812 setores); nenhum dos dois alcança os ARCs. O trace de controle reteve
os eventos de pad relevantes (o CSV tem 9.821 descartes antigos no total):
direita foi observada em OREG2=`0x7F` em 602 leituras, e a liberação em
`0xFF` em 22 leituras. A captura mostra o cenário lateral da fase, deslocado
pela direita; a imagem enviada pelo usuário continua sendo a abertura normal.

Na captura, a faixa RAM `0x06003000–0x060038FF` ainda coincide com
`IP.BIN+0x1000` em 2.032/2.304 bytes, e VDP1 VRAM em `0x60000–0x608FF` coincide
com a RAM em 2.304/2.304 bytes. Isso mantém a falha antes do DMA/renderização.
Próximo passo: identificar o callback/estado do jogo que deveria iniciar o
carregamento durante gameplay. Não alterar CD, SCU DMA ou VDP1 sem um pedido de
recurso observado. Artefatos: `build/artifacts/player-stage-archive-loader-
dedicated-20261018/`, `build/tmp/player-stage-archive-loader-dedicated-
20261018.txt` e `build/tmp/control-trace-x-stage-archive-loader-dedicated-
20261018.csv`.

## Probe de callbacks e replay divergente (2026-10-19)

O probe do dispatcher de objetos em `0x002223E2` foi filtrado para aceitar
somente chamadas em que o alvo coincide com `R3` e com a entrada da tabela
resolvida pelo mesmo ID de objeto. No replay filtrado, a tabela fica em
`0x00288DCC` e há 11 callbacks confirmados, mas todos param até o passo
285.794.331. Eles não identificam a rotina de atualização do jogador durante
a entrada direita de 330M.

Esta captura não reproduz a rota válida da fase: o quadro 103.654 tem os dois
bancos e o framebuffer VDP1 vazios, enquanto o quadro válido 97.100 tem 282
comandos VDP1 e 255 sprites. Portanto, não combinar os callbacks desse replay
com o sprite corrompido nem concluir que a fase não tem callback de jogador.
O dispatcher do aplicativo em `0x0603084C` é outra tabela, ligada ao caminho
de serviços assíncronos; não confundir com a tabela de objetos acima.

A imagem anexada pelo usuário é a abertura normal do jogo. Ela não corresponde
a nenhum desses quadros laterais da fase. Próxima prioridade: repetir a rota
válida da fase e capturar a tabela de objetos durante a janela de movimento,
preservando o filtro de chamadas confirmadas; então seguir o callback do
jogador até a rotina de solicitação de `ST0_1_0.ARC`/`ST0_1_1.ARC`.

O runtime SCU DMA também passou a executar listas indiretas (tripletos de
contagem, destino e origem, com o bit final de término) e ativa transferências
por VBlank-IN/OUT, HBlank-IN, Timer 0 e fim de desenho VDP1. A implementação
compilou em Release; replay com um descritor indireto real ainda é necessário
para validar o caminho. Timer 1 e pedido SCSP seguem sem evento no scheduler.

## Replays de abertura e rota de gameplay (2026-10-19)

A imagem anexada pelo usuário é da abertura cinematográfica. Uma captura com
Start extra em 70M parou no logo/título; a repetição sem esse Start chegou ao
hangar/transição, mas não à fase jogável. Ambas diferem da captura válida de
18/10, que tem 282 comandos VDP1 e 255 sprites. Os dois replays novos terminam
com 25 comandos e zero sprites, então nenhum deles serve para validar o sprite
de X.

O PLAY nº 15 também confirma a divergência: ambos iniciam em FAD 739 por 5
setores, enquanto a rota válida inicia em FAD 12.035 por 121 setores. Remover
o Start de 70M, por si só, não recupera o PLAY válido de FAD 14.249. Portanto,
o ponto em aberto continua sendo o estado que prepara os parâmetros desse PLAY
entre 100M e 155,7M; não inferir a sequência correta a partir da tela de abertura
nem dos quadros do hangar.

Na repetição sem Start extra, os 256 descritores de SCU observados usam
`mode=0x7`; nenhum ativa o bit 24 de DMA indireto. O build Release concluiu o
replay até 340M, mas esse resultado não valida a lista indireta com dados do
jogo. Captura e traces: `build/artifacts/player-stage-x-right-indirect-dma-
corrected-20261019/` (logo) e
`build/artifacts/player-stage-x-right-indirect-dma-baseline-route-20261019/`
(hangar). Próximo passo: recuperar o estado/rota que gera o PLAY válido,
registrar OREG2 e as escritas dos parâmetros do CD desde antes de 100M e
repetir a captura de gameplay; depois exercitar DMA indireto com um descritor
real e conferir se o jogo usa esse modo.

## Recuperação dos pulsos de entrada e proveniência (2026-10-19)

Os replays divergentes não usavam a mesma entrada da rota válida. O log
`player-stage-x-right-indirect-dma-baseline-route-20261019.log` mantém Start
pressionado de 13,7M até 24M e A depois disso; o log válido
`player-stage-archive-loader-dedicated-20261018.txt` libera Start em 14M,
pressiona novamente em 20M e libera em 20,3M, e também libera cada pulso de A.
Isso já difere em `smpc-pad` no checkpoint 14M, antes da divergência de PLAY.

A sequência foi recuperada do CSV `control-trace-x-right-full-0-160-20261018.csv`.
Com o executável existente antes do rebuild desta sessão, a repetição até 30M
coincidiu em todos os 29 checkpoints de progresso disponíveis e nos primeiros
20 PLAYs, incluindo o PLAY nº 15 em FAD 12035. Evidência:
`build/tmp/player-route-prefix-comparison-20261019.json` e
`build/artifacts/player-route-prefix-20261019-review/`. Esse é um prefixo de
rota; não é uma nova validação de gameplay até 340M.

O comando reproduzível está em
[`tools/diagnostics/replay_x4_stage.ps1`](../tools/diagnostics/replay_x4_stage.ps1).
Ele conserva as liberações, usa VBlank de 14000 blocos e grava argumentos e
SHA-256 do executável/AIP/IP/0.BIN/disco em um manifesto ao lado do log. O
manifesto também registra o status e o limite efetivamente alcançado. Exemplo:

```powershell
& tools/diagnostics/replay_x4_stage.ps1 -Name x4-stage-replay
```

O rebuild atual difere dos checkpoints do executável anterior já em 1M.
Uma comparação isolada de 1,1M entre dispatch estático com cache, dispatch
sem cache e interpretador coincidiu nos 11 campos registrados de CPU, CD,
vídeo, SCU e SCSP. Portanto, não atribuir essa diferença ao cache de dispatch
nem misturar os checkpoints antigos com a nova build. Comparação:
`build/tmp/static-cache-cosim-20261019.json`. O replay longo iniciado antes
dessa comparação (`player-stage-x-restored-static-cache-20261019`) foi
interrompido; seus arquivos não comprovam conclusão da rota.

Uma build de controle sem as alterações novas de cache/revisão SIR também
coincidiu nos 11 campos da comparação de 1,1M. A imagem mantém o SHA-256
`c1977179f386a2131cfdaf73417f4de8eb1f7fb984049518c2b2028330b09387` e os
arquivos locais 0.BIN/IP/AIP coincidem com os bytes do disco. Isso não identifica
a origem da diferença de timing para o executável anterior, mas evita atribuí-la
à otimização sem evidência.

## Replay da build atual até 160M (2026-10-19)

`player-stage-x-restored-prefix-20261019` terminou em `step-limit`, código de
saída 0, após 160.000.000 blocos: 143.466.385 estáticos e 16.533.615 JIT, sem
fallback interpretado. O quadro capturado em 155M mostra o título com
"PRESS START BUTTON"; a lista final contém 25 comandos e zero sprites. Não é
uma captura de gameplay nem uma validação da arte do jogador.

O PLAY nº 15 continua em FAD 739 por 5 setores, em vez do FAD 12035 por 121
setores da rota histórica. O trace reteve 37 PLAYs; o probe do caminho de ARC
registrou zero chamadas. Portanto, conservar as liberações dos botões recupera
a rota do executável antigo, mas não basta para recuperar a rota na build atual.
O próximo replay deve sincronizar Start com o título observado e registrar as
decisões do menu antes de voltar ao diagnóstico do buffer de textura de X.

Comando desta captura:

```powershell
& tools/diagnostics/replay_x4_stage.ps1 `
  -Name player-stage-x-restored-prefix-20261019 `
  -Steps 160000000 -CaptureAfterBlocks 155000000
```

Artefatos em `build/artifacts/player-stage-x-restored-prefix-20261019/`; log,
manifesto e trace de controles em `build/tmp/` com o mesmo nome. O script
reproduz uma sequência registrada; não certifica chegada à fase em outra build.

A otimização AOT reutiliza a seleção por endereço/assinatura enquanto a revisão
do corpo SIR não muda. O teste cobre substituição no mesmo endereço e tamanho,
além de um miss seguido de append. A suíte Release terminou com 117 testes
aprovados. Foram corrigidas três expectativas antigas dos testes de vídeo:
pixel vazio é alpha zero, números de character usam unidades de 32 bytes e
TVMD=0x8010 seleciona 240 linhas. O renderer não foi alterado nesta sessão.

## Produtor do sprite: trabalho do SH-2 escravo (2026-10-19)

A comparação offline dos traces completos de 18/10 e do replay atual de
160M localizou a primeira divergência no PLAY 15. Os 14 PLAYs anteriores
coincidem em FAD/tamanho. Na rota válida, o Start de 20M é lido como
OREG2=`0xF7` 37 vezes e a confirmação pede `STE_1_0.ARC` (FAD 12035).
No replay atual não há leitura do pad durante esse pulso; a última leitura
anterior é em 19.992.201 e a seguinte em 21.753.196. O PLAY em 20.038.357
pede `PLDEMO00.ARC` (FAD 739), seguido por `PL01.ARC` e pelo stream Zero
em FAD 18061. Isso identifica uma entrada na demonstração automática, não
uma seleção de arquivo X que o CD tenha trocado por Z.

`tools/diagnostics/analyze_x4_route.py` correlaciona os eventos do pad, as
leituras do bloco de periféricos `0x0603446C` e os PLAYs com os extents ISO.
O relatório é `build/tmp/x4-route-first-divergence-20261019.json`. Um teste
de 30M antecipou Start para 18M e as duas confirmações para 21M/24M: as
leituras OREG2 confirmam os pulsos, e os pedidos passam por `STE_1_0.ARC`,
`ST0E_0_1.BIN`, `ST0E_1_1.BIN` e `X1.CPK`. Arquivos:
`build/tmp/x4-route-title-before-timeout-20261019.json` e
`build/artifacts/player-x-title-before-timeout-20261019/`. É uma rota curta
equivalente; o histórico disponível não recupera os eventos SMPC da iteração
41 original. O script de replay agora usa esse prefixo e preserva os hashes
do executável e das entradas no manifesto.

A RAM sincronizada da iteração 41 contém um trabalho ainda pendente:

| Campo | Endereço | Valor |
| --- | --- | --- |
| Callback | `0x060604D0` | `0x06016F98` |
| Origem comprimida | `0x060604D8` | `0x060BE04E` |
| Destino descomprimido | `0x060604DC` | `0x06003000` |

O construtor em `0x0601B0A2` publica esses campos, escreve em MINIT
(`0x21000000`) e agenda o DMA. O programa escravo em `0x060116EE` espera
FTCSR.ICF, chama o callback pelo alias cache-through e limpa o trabalho.
A origem pertence ao tipo 2 de `PL00.ARC`: todos os `0x504` bytes consumidos
coincidem com o intervalo de arquivo `[0x380E,0x3D12)`. O callback original
produz exatamente `0x780` bytes em `[0x06003000,0x06003780)`.
Os descritores de pose em `PL00.ARC+0x878/+0x87C` valem `0x00F02FCA`:
os bits altos dão `15 × 128 = 0x780` bytes e os bits baixos dão a origem
`0x844 + 0x2FCA = 0x380E`, relativa ao banco de animação. Essa é a relação
entre a pose e o tamanho do DMA observada no código do construtor.
O `PL00.ARC` extraído coincide integralmente com o arquivo da imagem MDF:
FAD 759, 292 setores. A origem comprimida começa em FAD 766, offset `0x0E`
do setor. Verificação: `build/tmp/x4-pl00-disc-provenance-20261019.json`.
Não é necessário carregar `ST0_1_1.ARC` para reproduzir esse trabalho; o
mapeamento anterior entre tipo 35 e esse upload permanece sem evidência.

Foram corrigidos os aliases cache-through da WRAM alta/baixa e adicionado
`SlaveSh2`: execução cooperativa dos blocos SH-2 originais, com registradores,
pilha e periféricos internos separados e memória externa compartilhada.
`SYS_SETSINT(0x94, entry)` e SMPC SSHON/SSHOFF controlam a entrada; MINIT
publica a captura FRT. O protocolo segue o exemplo de polling de
[Using Dual CPUs on the SEGA SATURN, seções 4.3–7.3](https://antime.kapsi.fi/sega/files/Sattechs.pdf#page=91).
Este recorte ainda não implementa interrupções/exceções do escravo nem
timing de cache/barramento de duas CPUs.

O alvo opcional `saturn-x4-sprite-probe` reproduz o trabalho sobre a RAM da
iteração 41, usando o programa do escravo e sem incorporar bytes do jogo:

```powershell
# Compilar com o CMake configurado do projeto: --target saturn-x4-sprite-probe.
& build/tools/saturn-sh2/Release/saturn-x4-sprite-probe.exe `
  build/artifacts/player-test-iter41-start-stage/video-diagnostics/ram_high.bin `
  build/tmp/x4-iter41-probe-interpreter.bin
# Repetir com --native ao final para comparar o JIT com o interpretador.
```

`tools/diagnostics/validate_x4_sprite_job.py ram_high.bin PL00.ARC decoded.bin`
localiza o payload residente, verifica todos os bytes comprimidos e compara
o resultado com a decodificação independente. O teste com um byte do output
alterado foi rejeitado. Relatório:
`build/tmp/x4-iter41-source-validation-20261019.json`.

O trabalho é reconhecido como concluído após 2.645 blocos no modo cooperativo
(2.652 na primeira agenda de um bloco por passo). Os 1.920 bytes
coincidem integralmente entre interpretador, JIT e uma decodificação
independente do arquivo; SHA-256
`62f588c3fe16e56ae08dfdcf8ad7c8087fd3574bde4760c90773d3c5adbfd8b5`.
Uma reconstrução com as mesmas memórias VDP1/VDP2, registradores e command
list, substituindo apenas essa faixa de textura, mostra X azul e íntegro;
os pixels alterados ficam em `[149,176) × [146,194)`. São renders controlados
do snapshot, não uma nova captura de gameplay. Prova, folhas de tiles e
imagens: `build/artifacts/player-x-slave-job-proof-20261019/`.

A suíte Release aprovou 120 testes, incluindo aliases, contexto separado,
MINIT, reset, reinício e falha de resolução do escravo. O replay curto de 1,1M com trace seletivo
coincidiu com o trace completo em todos os nove dumps binários e no PPM;
relatório `build/tmp/x4-selective-capture-cosim-20261019.json`. O script
mantém o trace amplo disponível por `-TraceGame`, mas usa captura seletiva
por padrão para evitar custo desnecessário nas próximas execuções.

O launcher conserva os traces e termina com erro quando o escravo não resolve
uma entrada, em vez de abortar sem resumo. O smoke negativo sintético em
`build/tmp/slave-failure-cli-20261019.log` terminou com código 1 e preservou
o comando SMPC no CSV. O progresso seletivo registra passo e PC dos dois
processadores a cada milhão de blocos, sem habilitar o probe amplo.

### Corrida entre descompressão e DMA

O primeiro replay com escravo, `player-x-slave-selective-scene-20261019`,
terminou em 120M e capturou o quadro 37523 em 118M. Registrou `PL00.ARC`
em 46.274.342, `ST0_0_0.ARC` em 102.209.757 e `ST00_0_X.BIN` em
104.191.169, FAD 12965/5096 setores. A imagem mostra X azul, porém a validação
dos bytes encontrou uma mistura de poses: a textura VDP1 contém exatamente
`[0,0x194)` da pose de `PL00.ARC+0xBF8` e o restante da pose em `+0x16FD4`.
A RAM já contém a pose nova completa. Assim, esse replay é um contraexemplo
de upload parcial, não uma validação visual concluída.

O rateio inicial de um bloco escravo por bloco mestre não respeita a diferença
entre os tamanhos dos blocos e os helpers acelerados do mestre. O worker agora
cede ao ler FTCSR.ICF vazio; MINIT o retoma até a próxima leitura vazia. O
callback original limpa o mailbox e termina a escrita antes de devolver o
controle ao mestre. É um recorte cooperativo por eventos: **a duração do
serviço e a contenção dos dois SH-2 ainda não são modeladas em ciclos**.
O teste exige a saída completa já no primeiro retorno da chamada de serviço.

`tools/diagnostics/validate_x4_sprite_capture.py` verifica separadamente RAM,
escritas do último upload e a textura VDP1 contra poses inteiras descomprimidas
do arquivo. A verificação rejeita o replay com mistura de poses, embora a
imagem pareça visualmente melhor. O replay corrigido usa 108M, captura em 106M
e a mesma sequência de botões, antes de qualquer ampliação a outras fases.

### Captura corrigida e validação completa dos bytes

`player-x-slave-yield-scene-20261019` terminou com código 0, `step-limit`,
108.000.000 blocos mestres e 233.089 blocos escravos; não houve fallback
interpretado. O quadro 33671 foi capturado em 106M. A rota reteve todos os
eventos de controle/CD, sem descarte, e pediu `ST00_0_X.BIN` no passo
104.191.169. Não leu FAD 1975 nem 2249 durante esse recorte.

O mailbox está livre. A RAM contém a pose de `PL00.ARC+0x16FD4`, expandida
de 1.384 para 2.304 bytes (`0x900`), em `0x06003000`. O upload SCU em
105.979.634 copia essa faixa para `0x25C60000`. **Todos os 2.304 bytes da
RAM e da VRAM capturada coincidem com a descompressão independente.**
SHA-256 da textura:
`4dba380897a2de69d7ca524db0e2457083f408efbed73a7c013b84261ecb7880`.

O CSV de writes VDP1 observa apenas o prefixo `0x300` da página; o validador
obtém o tamanho completo no evento SCU de mesmo passo e compara a faixa
inteira da VRAM com o arquivo. Ele aceita a captura corrigida e rejeita a
mistura de poses do primeiro replay.

Artefatos: `build/artifacts/player-x-slave-yield-scene-20261019/`, incluindo
`captured-frame.png`, `texture-validation.json`, a trilha DMA e os dumps
sincronizados. O conjunto ocupa aproximadamente 10,95 MB. Manifesto com os
hashes das entradas, log e trace de controle estão em `build/tmp/` com o
mesmo nome. A rota está em `build/tmp/x4-final-route-20261019.json`.

```powershell
& tools/diagnostics/replay_x4_stage.ps1 `
  -Name player-x-slave-yield-repeat -Steps 108000000 -CaptureAfterBlocks 106000000
python tools/diagnostics/validate_x4_sprite_capture.py `
  build/artifacts/player-x-slave-yield-repeat build/rockman/PL00.ARC
```

### Validação temporal de troca de overlay (2026-10-25)

A rota de X foi repetida até 160M passos com período VBlank 14.000, captura
em 106M e a mesma sequência do pad. O trace reteve 194.261 eventos sem perda;
`right` foi aplicado de 122M a 145M. A captura em 106M tem o mesmo SHA-256 da
execução anterior. O CD pediu `ST00_0_X.BIN` em FAD 12965 no passo 104.191.169
e repetiu o mesmo intervalo em 138.295.169 e 153.387.169; os FADs 1975/2249
não foram solicitados.

O resolvedor fez 13 re-lowerings até o passo 104.931.074. Todos registram
`old=unknown`, pois não havia fingerprint de bytes ativos anterior para esses
blocos SIR já materializados. Nenhuma mudança de versão de código com assinatura
anterior conhecida nem rebind AOT ocorreu durante o `right` de 122M–145M. Esta
execução, portanto, **não confirma uma troca real de overlay em gameplay**; ela
confirma apenas a primeira resolução dos blocos da cena X. O caminho dedicado
do handler de ARC, `0x00271952`, continua sem execução nesta rota, e ainda não
há leitura dos FADs dos ARCs da fase.

Artefatos: `build/artifacts/player-x-overlay-validation-steps2-20261025/`,
`build/tmp/player-x-overlay-validation-steps2-20261025.log`, o manifesto JSON
homônimo e `build/tmp/control-trace-player-x-overlay-validation-steps2-20261025.csv`.

### Extração estrutural de sprites e metasprites (2026-10-25)

Sem repetir o replay, extraí as poses dos bancos tipo 2 de `PL00/PL01/PL02`
com `tools/diagnostics/extract_x4_sprite_banks.py`. Os 728 descritores
correspondem a 408 fontes distintas; todos os tamanhos descomprimidos conferem
com os descritores e os streams permanecem dentro dos payloads. Os arquivos e
manifesto estão em `build/rockman/sprite-banks/`.

Também exportei os registros de peças dos payloads tipo 1 de `ST0_0_0.ARC` e
`ST0_1_0.ARC` via `tools/diagnostics/extract_x4_type1_frames.py`: 14 bancos por
arquivo, 704 quadros e 17.657 registros contando aliases. Os limites e a
continuidade fecham em todos os bancos; os bancos 11 e 12 de `ST0_0_0` são um
alias. A primeira saída ficou em `build/rockman/arc-decoded/`; a varredura
ampliada abaixo substitui esse recorte para análise geral. Os campos gráficos
foram preservados como palavras BE16 brutas: ainda não atribuí significado a
flags, base do atlas ou coordenadas.

REA foi consultado, mas seu provider não aceita o formato proprietário `.ARC`
nem os payloads binários extraídos. A leitura acima vem dos parsers locais e
das comparações já ancoradas no artefato; não houve nova captura de gameplay.

### Pacote visualizável dos ARC (2026-10-25)

`tools/diagnostics/preview_x4_arc_graphics.py` gerou 102 folhas em
`build/rockman/arc-previews-expanded/`: type 5 (45), type 14 (39), type 17 (3),
type 27 (3) e type 35 (12).
Os pixels são índices 4bpp em tiles 16x16, organizados em 16 colunas e exibidos
em escala de cinza; a paleta original não está aplicada. As folhas tipo 14 e 35
reproduzem a disposição das contact sheets anteriores.

`tools/diagnostics/package_x4_arc_analysis.py` agrega payloads byte-exatos,
poses/frame tables e previews em `build/rockman/x4-arc-analysis.zip`. REA abriu
esse ZIP e inventariou 959 nós e 1.093 ocorrências. A análise do provider se
limita ao diretório ZIP; a interpretação interna continua apoiada nos parsers
locais e nas proveniências do manifesto.

### Expansão dos bancos tipo 1 (2026-10-25)

Ampliei o parser tipo 1 aos 48 arquivos ARC que têm esse payload. Quarenta e
seis passaram pelas verificações de offsets, continuidade dos registros e
fechamento exato dos bancos: 311 referências a bancos (308 bancos únicos),
13.874 quadros e 216.596 peças incluindo aliases. `STE_0_0.ARC` e
`STF_1_0.ARC` não correspondem ao formato reconhecido; seus payloads foram
mantidos em bruto com o erro do parser em `build/rockman/arc-decoded-all/`.
O pacote final, após a ampliação dos previews, foi reindexado pelo REA: 959 nós
e 1.093 ocorrências.
Isso evita tratar duas variantes ainda desconhecidas como se fossem quadros
decodificados.

### Controle do jogador e lançamento interativo (2026-10-25)

O replay de 160M permite avançar o diagnóstico sem nova execução longa. A única
imagem sincronizada continua sendo o quadro 33671 em 106M, antes da pressão de
`right` entre 122M e 145M. O trace mostra que o SMPC recebe o estado e o jogo o
lê: `0x20100025` passa de `0xFF` para `0x7F` durante a pressão e volta a `0xFF`
na soltura. Isso confirma entrega até o leitor de periféricos, mas sozinho não
prova deslocamento de X.

A inspeção estática do dump de RAM sincronizado localizou a continuação do
caminho: o leitor em `0x06010CDE` consulta a tabela de periféricos e normaliza
os bits ativos-baixos para o estado em `0x0605E9FC`. A rotina de atualização
em `0x060249D8`, chamada pelo código de jogo em `0x00221FEE`, remapeia os bits e
grava held/previous/pressed no objeto principal em `0x060545A0` (`+0x7C`,
`+0x7E`, `+0x80`). O bit raw de direita (`0x8000`) vira o bit interno `1`.
O loop de jogo chama esse remapeamento e depois o dispatcher do jogador em
`0x060264A8`. O handler `0x06024E40/0x06024E6C` lê `obj+0x7C`: direita escolhe
direção `0x10` e velocidade X `0x00020000` (ou `0x00042000` se `obj+0x84` não
for zero). O bloqueio direcional usa os bits 1/2 de `obj+0x89`; a integração
em `0x06022A2C` atualiza `obj+8` pela velocidade em `obj+0x20`. O formato
16.16 é uma inferência forte a partir dos valores e operações.

O dump usado como baseline contém posição `X=0x00C80000`, `Y=0x01AB0000`,
velocidade X zero, `obj+0xC3=0`, `obj+0x89=0x08` e `obj+0x84=0`. Nesse estado
inicial, os gates conhecidos não bloqueiam direita. O replay posterior descrito
abaixo confirmou em runtime que o bit chega ao handler e seleciona a velocidade
esperada; ainda falta comparar posições antes e depois na mesma execução.

Há também um bloqueio do caminho interativo: `--pad-sequence` marcava a porta 1
como scripted por toda a sessão, então teclado/XInput nunca assumia após a
sequência de navegação. O CLI agora oferece `--pad-live-after-blocks`; ele solta
a porta 1 e devolve o controle humano ao frontend no passo indicado. O novo
`tools/diagnostics/launch_x4_stage.ps1` reaproveita a rota já confirmada e passa
o controle ao usuário em 160M, após a transição READY. O caminho com eventos
programados foi exercitado no replay abaixo; ainda falta experimentar o
launcher com teclado ou XInput físico. Não existe save-state completo para
retomar a sessão salva em 106M.

Artefatos da análise estática: `build/tmp/control-trace-player-x-overlay-validation-steps2-20261025.csv`,
`build/artifacts/player-x-overlay-validation-steps2-20261025/video-diagnostics/ram_high.bin`
e `frame_033671_vdp1-command-list.txt`.

### Input e textura durante a cena (2026-10-25)

O replay `player-x-gameplay-right-20261025` reaproveitou a rota, pressionou
direita entre 160M e 170M e capturou o quadro 52997 em 168M. No snapshot,
OREG2 do SMPC está em `0x7F` (direita ativa), o estado normalizado em
`0x0605EA00` contém o bit raw `0x8000`, e o objeto remapeado em `0x0605461C`
contém held=`1`. O objeto do jogador tem `obj+0x7C=1`, `obj+0x20=0x00020000`,
`obj+0xC3=0`, `obj+0x89=0x08` e `obj+0x84=0`. Isso confirma a passagem da
entrada pelo leitor, remapeamento e handler de movimento até a velocidade de
direita. Como há apenas um dump de posição nessa execução, ainda não quantifica
o deslocamento resultante.

O validador independente `tools/diagnostics/validate_x4_sprite_capture.py`
confirma que o worker estava livre e que `0x06003000` continha 2.688 bytes
descomprimidos de `PL00.ARC`, com origem comprimida em `0x060BF2A6` e offset
`0x4A66` no payload tipo 2. O DMA de passo 167.999.788 copiou `0xA80` bytes de
`0x06003000` para `0x25C60000`. Os bytes completos na VRAM correspondem a uma
pose inteira de `PL00.ARC+0x4318` (índices de animação 17 e 18 são aliases); o
prefixo também confere com os writes registrados no barramento. A RAM no dump
já contém outra pose completa, de `+0x4A66`, portanto o buffer de origem foi
reutilizado depois do upload. A diferença entre RAM atual e VRAM não é evidência
de upload parcial.

O quadro tem 224 comandos VDP1, saída de vídeo ativa e 76.800 pixels não pretos.
Isso confirma uma cena renderizada, mas não a troca de overlay: o histórico de
CD desta execução não solicita FAD 1975 ou 2249 (`ST0_1_0.ARC` e
`ST0_1_1.ARC`). Logo, o tipo 35 de `ST0_1_1.ARC` continua sem ligação provada
com esse upload de `0xA80` bytes; esse upload específico veio de `PL00.ARC`.

Artefatos: `build/artifacts/player-x-gameplay-right-20261025/captured-frame.png`,
`build/artifacts/player-x-gameplay-right-20261025/texture-validation.json`,
`build/artifacts/player-x-gameplay-right-20261025/video-diagnostics/` e
`build/tmp/control-trace-player-x-gameplay-right-20261025.csv`. Próximo passo:
usar o launcher interativo para confirmar deslocamento em cena e seguir a
transição que efetivamente solicita os dois overlays; só então mapear os
produtores e destinos de textura deles.

### Perfil em tempo real e áudio (2026-10-26)

O teste interativo confirmou que o renderer CPU não sustenta a cadência: em
uma sessão limitada, a média foi 38,28 FPS com 323 underruns de áudio. Vulkan
melhorou para 46,57 FPS e 62 underruns com o VBlank de 14.000 blocos. A janela
e a cópia do renderer não são o custo dominante (pintura média de 0,20 ms;
cópias Vulkan de 0,93 ms por quadro). A build Release executou 13.691 blocos
nativos estáticos; uma build de comparação só com JIT encadeado ficou em
25,50 FPS no menu.

O frontend agora aplica a reserva inicial de 50 ms apenas ao iniciar o áudio.
Após um underrun, reinicia na chegada do primeiro bloco PCM, evitando inserir
outra pausa de 50 ms. Com o perfil Vulkan e `--vblank-blocks 10000`, um replay
de 30M terminou no limite previsto, alcançou o handoff e registrou média de
58,34 FPS, 6 underruns, zero erros de escrita e 3.000 quadros apresentados.
O launcher de `tools/diagnostics/launch_x4_stage.ps1` aceita
`-VBlankBlocks` e usa 10.000 como padrão para este host; a CLI continua com
14.000 por padrão.

A rota interativa com esse perfil chegou à primeira cena da fase, com X visível
e Vulkan entre 53 e 56 FPS durante a cena. Um toque de seta foi enviado após o
handoff estimado, mas a captura visual não basta para confirmar deslocamento;
o teste de movimento com posição inicial/final continua pendente. A execução
completa até o handoff ainda não foi encerrada, então não há métricas finais da
rota. A troca de overlay via FAD 1975/2249 segue sem confirmação.

### Relógio de áudio em tempo real (2026-10-26)

O mixer emitia sempre 735 amostras por VBlank, embora essa quantidade represente
apenas 1/60 s. Quando o jogo cai para 40 quadros/s, o cursor do PCM avança só
29.400 amostras por segundo: a música fica lenta e a fila do `waveOut` perde
blocos. O modo interativo agora mede o tempo real entre VBlanks e gera 44.100
amostras por segundo, mantendo a saída de testes determinísticos no bloco fixo de
735. Os buffers do frontend foram ampliados para receber até 500 ms por bloco;
eventos de áudio com atrasos maiores continuam limitados e podem ainda engasgar.
Esta mudança corrige o relógio do áudio, mas não aumenta a capacidade de CPU que
limita a imagem a cerca de 40 FPS na cena pesada; o próximo replay precisa
confirmar a audição e as métricas de underrun no launcher atualizado.

### Recuperação de pintura e áudio (2026-10-26)

Na sessão ao vivo, três capturas consecutivas mostraram fundo e HUD estáveis; o
grande padrão amarelo/preto se move com a animação. Não apareceu um quadro preto
completo nessas amostras. O padrão é compatível com o modo mesh do VDP1, que
descarta pixels alternados, mas ainda não foi correlacionado com o comando VDP1
que o produz. O frontend agora compõe a janela em um bitmap de apoio e publica
cada pintura com um único `BitBlt`, evitando expor o preenchimento preto entre
quadros. O título da janela passa a mostrar underruns e blocos de áudio cortados.

Uma medição Vulkan anterior registrou intervalo máximo entre apresentações de
198 ms. Como o clock de áudio anterior cortava blocos acima de 100 ms, essa
mudança elevou a capacidade para 500 ms; isso cobre os picos observados sem
descartar o trecho correspondente da música. A sessão aberta naquela etapa usava
o binário anterior e precisava de reinício para avaliar essas duas mudanças. O
binário Release foi compilado em `build-jit` para preservar aquela sessão, e o
launcher aceita `-ExecutablePath` para selecioná-lo. O CTest passou (1/1),
incluindo a verificação do clock de áudio em tempo real.

### Efeitos de gameplay e comandos sonoros (2026-10-26)

A gravação fornecida pelo jogador mostra disparos, dash, efeitos pontilhados e
efeitos de impacto durante a fase. Uma captura anterior da mesma rota usava
TVMD `0x8010`, com saída de 240 linhas; o shader Vulkan só processava as
linhas 0–223. Agora ele cobre as 256 linhas do framebuffer e respeita o recorte
de cada primitiva. Um teste desenha o mesmo sprite nas linhas inferiores de
modos de 224, 240 e 256 linhas e compara CPU com Vulkan: falhava antes da correção
e passou depois. Isso corrige uma diferença específica; não prova que todos os
efeitos ou sombreamentos mostrados no vídeo estejam corretos.

O snapshot `build/artifacts/player-x-gameplay-right-20261025/video-diagnostics/scsp_ram.bin`
mostra na fila, entre outros, `01 00 06 03 02 ...` e
`8A 00 00 E0 00 00 00 00 0C ...`. Segundo o
[manual do driver da Sega](https://www.infochunk.com/saturn/segahtml_en/sndt/prog/drve/sndcmd_.htm),
`01` inicia uma sequência e `8A` altera os parâmetros de PCM. O primeiro
continua sem implementação: tocar efeitos ou vozes de sequência exige o driver
68K/SCSP ou uma implementação equivalente. As contagens antigas de comandos
incluem reobservações de pacotes ainda pendentes, e não devem ser tratadas como
quantidade de efeitos acionados.

O runtime agora aplica `8A` quando os parâmetros não solicitam entrada no DSP,
alterando nível, pan e pitch do PCM ativo sem reiniciar o cursor. O nível total
também é aplicado no `85` inicial. Pedidos que exigem DSP permanecem pendentes;
o teste unitário verifica mudança de pitch e nível, continuidade do cursor e
essa limitação. Isso não adiciona reprodução de sequências nem confirma que a
voz específica observada no vídeo utilize PCM em vez de sequência.

O replay headless de 30 milhões de blocos com Vulkan e VBlank de 10.000 chegou
ao handoff e ao limite esperado, sem fallback de interpretador. Registrou sete
inícios PCM e dois avistamentos de `8A`, contra 1.120 reobservações no replay
anterior em tempo real. O frame final tinha 53.440 pixels não pretos em ambos os
replays; os relógios de áudio diferem, então essa comparação não prova que os
dois estados ou o áudio completo sejam idênticos.

Os insumos originais `SDDRVS.TSK` (26.610 bytes) e `SDDRVS.MP` (884 bytes)
já estão extraídos. Os arquivos `SNDCOM.ARC`, `SEPL00.ARC` e `SEPL01.ARC` foram
separados em `build/rockman/sound-arc-extracted-20261026/`: quatro payloads
brutos, com offsets e SHA-256 no manifesto. Na captura SCSP citada acima, o
payload tipo 20 de `SNDCOM` (325.874 bytes) coincide integralmente com a sound
RAM em `0xB000`, e o tipo 21 de `SEPL00` (198.150 bytes) coincide com a RAM em
`0x2A2EC`. Este último também é exatamente o final do payload `SNDCOM`, a partir
do offset `0x1F2EC`. Isso comprova a presença desses bytes na execução capturada;
a identificação de instrumentos, sons específicos e sequência ainda está pendente.

O snapshot de sound RAM também coincide com `SDDRVS.TSK` em 26.591 dos
26.610 bytes. As 19 diferenças ficam no indicador `0x4E1` e nos pacotes/status
da interface `0x700–0x7A0`; não há diferença nos demais bytes do arquivo. Os
dois primeiros longwords do driver são `0x0000A000` e `0x00001000`, consistentes
com a pilha inicial e o PC de reset do 68000. Assim, o jogo carregou o driver;
o runtime atual é que ainda não executa esse código.

O `SDDRVS.MP` descreve no mapa 0 cinco bancos do tipo sequência. No snapshot,
o pacote `08 00 00 ...` solicita o mapa 0, mas permanece pendente; portanto não
é evidência de que o driver tenha ativado esse mapa. Sob esse mapa solicitado,
o pacote `01 00 06 03 02 ...` seleciona controle 6, banco 3 e sequência 2.
O banco 3 ocupa `0x29A1A–0x29E99`, tem 25 offsets válidos e a entrada 2 ocupa
42 bytes a partir de `0x29AD4` (SHA-256
`c39b9b3303df5000fd7a72ca85c3d3b61532fe32562006a09914c6f5629f4040`).
As tabelas dos cinco bancos contêm 113 entradas. O extrator
`tools/diagnostics/extract_x4_sound_banks.py` gravou os bytes em
`build/rockman/sound-sequences-map0-20261026/` e verificamos cada banco contra a
sound RAM e contra o payload bruto `SNDCOM`. Os eventos comprimidos ainda não
foram decodificados; não há identificação auditiva do som dessa entrada.
As 25 entradas do banco 3 têm 42 bytes e são idênticas exceto pelo byte no
offset `0x24`, que varia de 0 a 24. Isso delimita uma família de solicitações,
mas o significado desse byte ainda precisa ser verificado no driver de som.

A gravação `20261027-0230-28.1194439.mp4` tem 62,1 s a 30 FPS. Ao reduzir os
quadros para 160x120 e classificar pixels como escuros quando os três canais
são menores que 12, 34 de 1.863 quadros têm mais de 90% de pixels escuros,
em 26 sequências de um a três quadros. Por exemplo, em 8,9 s há dois quadros
pretos entre imagens normais de combate. Isso confirma o sintoma no vídeo e
substitui a conclusão limitada das três capturas pontuais anteriores. A
gravação sozinha não identifica se o quadro escuro veio do runtime de vídeo ou
da pintura da janela. O título e o resumo do frontend agora contam quadros de
entrada com mais de 90% de pixels escuros; comparar o contador durante um flash
ajudará a distinguir essas duas origens. Transições normais também o incrementam.

O replay headless com Vulkan, VBlank de 14.000 e controle à direita entre
160M–170M chegou a 171 milhões de blocos e ao handoff, com zero fallback de
interpretador. A captura em 168M gerou `captured-frame.png` em
`build/artifacts/pcm8a-vdp1-gameplay-20261026/`, na fase de X em saída de
320x240; o estado final tinha 207 sprites e nove polígonos. No snapshot SCSP,
o comando `8A` direto havia sido limpo, mas `08`, `87` e a sequência
`01 00 03 02 0B ...` ainda estavam pendentes. Sob o mapa 0 solicitado, esse
último comando aponta para a entrada 11 do banco 2, em `0x29876`, cujos 42 bytes
conferem com a extração anterior. É uma única imagem, sem exercício específico
de ataque ou dash: não valida o piscar nem a fidelidade dos efeitos em movimento.
As builds Release em `build` e `build-jit` foram atualizadas, e o launcher em
tempo real foi aberto com os contadores de quadro escuro e áudio no título.

### Compatibilidade do host atual (2026-10-27)

A máquina de desenvolvimento foi identificada como Intel Core i5-4590
(Haswell, 4ª geração), Intel HD Graphics 4600, driver 20.19.15.4531. O loader
Vulkan do Windows não está instalado. O guia Vulkan da Intel define a 6ª
geração como requisito mínimo para o caminho Vulkan em gráficos Intel
([guia Vulkan da Intel](https://www.intel.com/content/dam/develop/external/us/en/documents/vulkan-on-unity-instructions-final.pdf));
portanto, esta máquina não tem um dispositivo Vulkan Intel oficialmente
suportado. Instalar apenas o loader não acrescentaria suporte de driver, e
`--renderer auto` usa o renderer CPU. As métricas Vulkan históricas do checkpoint
não são reproduzíveis nesta máquina; as próximas medições de imagem e áudio
devem usar o caminho CPU. A execução do driver SCSP 68K para sequências e vozes
continua sendo trabalho separado da escolha do renderer.

### Posição de X antes/depois de direita (2026-10-27)

Foi acrescentada a opção `--x4-player-samples <csv>` ao `saturn-sh2`. Em cada
evento de `--pad-sequence`, ela registra o estado **antes** de aplicar a nova
entrada, lendo RAM diretamente para não passar pelos interceptores. As colunas
incluem bloco e quadro atuais, pad anterior e próximo, `obj+8`, velocidade X,
held/previous/pressed e os gates `obj+0x84`, `obj+0x89` e `obj+0xC3`.

O replay partiu do boot, usou VBlank de 14.000 blocos e renderer CPU. A rota
programou direita em 160M e soltura em 170M; terminou no limite de 172M, após o
handoff. As duas amostras relevantes foram feitas nos passos exatos:

| Passo/fase | Pad anterior → próximo | `obj+8` X | Velocidade X | Held | Gates 84/89/C3 |
| --- | --- | ---: | ---: | ---: | ---: |
| 160.000.000, antes de pressionar | `0x0000 → 0x0001` | `0x00C80000` | `0x00000000` | `0x0000` | `0x00 / 0x08 / 0x00` |
| 170.000.000, antes de soltar | `0x0001 → 0x0000` | `0x08A50000` | `0x00020000` | `0x0001` | `0x00 / 0x09 / 0x00` |

O delta bruto é `+0x07DD0000`, igual a `+2013` unidades sob a interpretação
16.16. Isso confirma que o campo de posição avançou durante a pressão de
direita. O formato 16.16 é uma inferência forte; a relação exata entre essa
coordenada e a rolagem da câmera ainda não foi estabelecida. Em `obj+0x89`, os
bits de bloqueio conhecidos (1 e 2) não estão ativos nas duas amostras; `obj+
0x84` e `obj+0xC3` permanecem zero.

A captura no passo 168M tem 320×240 pixels, saída ativa e 76.800 pixels não
pretos. O resumo chegou a `step-limit` em 172M com handoff; executou
49.652.178 blocos nativos estáticos e 128.571.986 blocos JIT, sem fallback do
interpretador. O slave executou 6.224.164 blocos.

Artefatos: `build/tmp/x4-player-position-20261027-run1.csv` (22 amostras),
`build/tmp/control-trace-player-x-position-20261027-run1.csv`,
`build/artifacts/player-x-position-20261027-run1/captured-frame.ppm`,
`build/artifacts/player-x-position-20261027-run1/video-diagnostics/ram_high.bin`,
`build/rockman/player-route-20261027-run1-entries.txt` e o módulo observado
`build/rockman/player-route-20261027-run1-module.cpp`. O movimento programado
está confirmado. O teste posterior com controle ao vivo também foi realizado;
ver o [diagnóstico da sessão](ROCKMAN-X4-LIVE-GLITCH-AUDIT.md).
