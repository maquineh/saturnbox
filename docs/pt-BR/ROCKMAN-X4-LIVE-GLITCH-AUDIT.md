# Rockman X4 — diagnóstico da sessão ao vivo de 27/10/2026

O usuário controlou X, concluiu a fase inicial e avançou pela fase de Frost
Kibatodos. A gravação confirma defeitos visuais em movimento e o usuário
relatou ausência dos sons de tiro, WARNING, impacto e explosão. A inspeção
encontrou lacunas concretas no VDP1, VDP2 e SCSP; a ligação de cada lacuna a
cada quadro ainda precisa de captura sincronizada do estado interno.

## Sessão e evidências

- Vídeo: `C:/Users/rapin/Videos/2026-10-27 19-18-20.mkv`, 27:28,91,
  H.264 1920×1080 a 60 FPS, AAC estéreo a 48 kHz.
- Runtime: renderer CPU, VBlank de 14.000 blocos, controle ao vivo no port 1
  depois de 160M blocos. OBS capturou a janela e o áudio do processo; desktop
  e microfone estavam silenciados.
- Diretório: `build/sessions/live-20261027-01/`. Contém `session.json`,
  `game.log`, `markers.csv`, `telemetry.jsonl`, `evidence/` e `analysis/`.
- Os tempos abaixo são posições no vídeo. Os tempos dos mosaicos extraídos
  são amostras nominais; não são timestamps de execução do VDP1.

## Sintomas confirmados e hipóteses

| Sintoma | Evidência | Diagnóstico e limite |
| --- | --- | --- |
| Poses do X acumuladas durante salto/tiro; usuário também observa no dash | 26:56–26:58; `analysis/shot-confirmed-contact-sheet.png` e `evidence/shot-duplication-user.png` | Várias poses completas/opacas aparecem empilhadas. Persistência de pixels no framebuffer é uma hipótese forte. A imagem não prova se esses pixels já estavam no banco ou se foram emitidos novamente pela lista atual. |
| Retângulos listrados junto ao X | 14:54,925–14:55,175, aproximadamente; `analysis/hit-contact-sheet.png` | Defeito localizado de textura/desenho, com fundo e HUD coerentes. Desaparece nas amostras seguintes. Pode ser separado do acúmulo de poses; não há prova de DMA parcial ou de descompressão incorreta nesse instante. |
| X aparece na frente do pilar que deveria encobri-lo | Referência fornecida pelo usuário: `evidence/occlusion-user-reference.png` | Erro de oclusão confirmado pela expectativa informada pelo usuário. Falta implementar prioridade especial de VDP2; ainda falta ler o bit do tile exato sob o pilar para fechar a causa desse caso. |
| Explosões/fumaça deformadas ou esticadas | Observação ao vivo, confirmação do usuário e `evidence/stretched-explosion-1244.96.jpg` | Defeito visual registrado. Restos de framebuffer e amostragem/geometria do sprite precisam ser discriminados com comandos e textura do mesmo quadro. |
| Tiros, WARNING, impactos e explosões sem som | Relato do usuário; comandos SCSP pendentes e implementação examinada | O caminho de sequências do driver de som não é executado. É uma limitação concreta compatível com os sons ausentes; os IDs de cada efeito ainda não foram correlacionados aos pedidos. |

Rastros intencionais do dash e a malha alternada habilitada por PMOD não
devem ser classificados como defeito só por conterem cópias ou pixels vazios.
As poses opacas acumuladas e os retângulos listrados acima são evidências
distintas. Esta inspeção não demonstra defeito de colisão/hitbox.

## Prioridade especial de VDP2 (implementada em 29/10)

O caso "X na frente do pilar" era a lacuna registrada como prioridade especial de
VDP2. A captura de diagnóstico do estágio (`frame_069705_registers.bin`, bloco
294M) mostra o registro **SFPRMD (`0x0EA`) = `0x0015`**: modo 1 em NBG0, NBG1 e
NBG2 (bits 1:0, 3:2 e 5:4), modo 0 em NBG3 e RBG0. O jogo usa mesmo o recurso —
e o emulador o ignorava, então o pilar ficava com a prioridade do registro (4)
empatado com o sprite de X, e o desempate dava o sprite na frente.

Implementação em `libs/saturn/src/video.cpp`:

- `composeVdp2Cell` grava, por dot, o bit de prioridade especial vindo do
  pattern name (bit 15 da palavra única, ou bit 15 da palavra inferior no
  formato de duas palavras). No modo 2 o bit só vale quando o código de cor do
  dot casa com o código de função especial.
- `mergeLayer` troca apenas o **bit menos significativo** da prioridade da
  camada (`(registro & ~1) | bit`), mantendo os dois bits superiores do
  registro, e trata prioridade resultante zero como transparente.
- RBG0 e as camadas bitmap continuam em modo 0; o modo 1 bitmap (bit do
  registro de paleta) ainda não é lido.

O teste `saturn: VDP2 special priority lifts cell priority over sprites` monta
uma camada de células em 256 cores e um sprite de mesma prioridade: no modo 0 o
sprite vence, no modo 1 com o bit do pattern name ligado a célula sobe para 5 e
cobre o sprite, com o bit desligado o sprite volta a vencer, e com prioridade de
registro 1 e bit 0 o dot fica transparente.

## VDP1: troca e apagamento dos framebuffers

Em `libs/saturn/src/video.cpp:710–740`, `startVdp1Draw()` arma a troca sempre
que FBCR contém `3`. Uma nova escrita de `3` em FBCR não arma a troca por si
só. `presentFrame()` consome a troca pendente em `video.cpp:232`.

O [manual de FBCR do VDP1](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p04_12.htm)
define a escrita do pedido manual de troca para o próximo campo. O pedido e
o disparo de desenho são operações distintas. O acoplamento atual ao desenho
é uma divergência concreta, mas não basta para atribuir todas as duplicações
a ela. A atribuição `vdp1ErasePending_ = ...` também substitui a intenção
pendente a cada escrita; é necessário observar a sequência real de escritas
antes de concluir que houve cancelamento indevido do apagamento.

Uma captura anterior, **não sincronizada com o defeito ao vivo**, contém um
polígono de cor zero cobrindo somente Y=110–239:
`build/artifacts/player-x-position-20261027-run1/video-diagnostics/frame_053015_vdp1-command-list.txt`.
O acúmulo do X na nova imagem coincide aproximadamente com a região superior
que esse comando não cobriria. É uma pista para inspecionar ambos os bancos,
não prova de que aquela lista era a lista ativa às 26:57.

## VDP2: correção de leitura da prioridade especial (30/10)

A implementação inicial de 29/10 consultava SFPRMD, mas lia o bit da origem
errada. Em pattern names de duas palavras, o bit especial é o bit 13 da palavra
superior; em entradas de uma palavra, vem do bit 9 do PNCN. A implementação
anterior usava o bit 15 da palavra inferior/entrada e ignorava as peças de
foreground vistas na captura 53.015. A leitura foi corrigida em
`libs/saturn/src/video.cpp`; o bit de `BMPNA` também alimenta a prioridade das
camadas bitmap.

Uma camada com prioridade-base zero também não pode ser descartada antes de
SFPRMD: com bit especial 1, sua prioridade calculada é 1 e o pixel é visível.
Os padrões do pilar na captura 53.015 incluem `0x2044` no NBG0; com `PRINA=4` e
`SFPRMD=0x0015`, o bit 13 eleva a prioridade para 5, acima da prioridade 4 do X.
Nas referências novas de 30/10, o pilar cobre o personagem onde os dois se
cruzam; essa é a relação de oclusão esperada. O replay atualizado em
`build/artifacts/x4-red-trail-replay-20261030/` também mostra o personagem atrás
do pilar nesse ponto.

## Pixels coloridos durante o movimento (30/10)

O usuário esclareceu que os pixels/trilhas mudam de cor conforme a posição do
X e aparecem quando ele se desloca. O compositor e a janela são refeitos por
quadro; não há persistência no bitmap do frontend. A reconstrução de uma pose
capturada é coerente, mas não contém os pixels fora do lugar da imagem enviada.

O replay controlado segurou direita entre os blocos 160M e 170M, capturou o
bloco 168M e concluiu em 172M (`step-limit`, saída 0). O quadro 53.075 mostra
o X atrás do pilar, mas não reproduz as trilhas coloridas das imagens do usuário.
O banco exibido tem 28.927 pixels não vazios; o outro banco está vazio após a
troca. Artefatos: `build/artifacts/x4-red-trail-replay-20261030/` e
`build/tmp/x4-red-trail-replay-20261030.log`.

O histórico local tem duas escritas `PTMR=1` por campo, com ambas as listas
desenhadas no mesmo banco antes da troca. O HLE termina cada lista
sincronamente, e a limpeza VBE ocorre somente na troca; a lista capturada
também inclui um polígono zero que cobre Y=110–239. A acumulação de partes da
pose acima de Y=110 continua possível, mas esse replay não comprova que seja a
causa dos pixels nas imagens novas. Não é seguro limpar o banco em toda escrita
PTMR, limitar a segunda lista ou filtrar uma cor sem observar o mesmo quadro
antes e depois da segunda lista.

## Áudio, desempenho e captura

O resumo final do frontend registrou 33.753 quadros, média de 18,5453 FPS,
sete underruns acumulados, zero erros de escrita e zero blocos PCM truncados.
O contador já estava em dois no início da gravação. Os 52.334 `audio waits`
são esperas por espaço na fila, não uma contagem de falhas audíveis.

Em `libs/saturn/src/scsp.cpp:168–237`, PCM direto e alguns parâmetros são
modelados, mas síntese de sequências e DSP estão ausentes. O driver 68K
carregado não é executado. O log observou `01`, `05`, `08`, `0E` e `87`, além
dos comandos PCM suportados. Esses contadores podem contar reobservações de
pacotes pendentes; não provam execução ou reprodução de milhares de sons.

A análise de amplitude encontrou aproximadamente 198 ms de quase silêncio em
11:35,569–11:35,767, próximo do aumento de underruns de cinco para seis.
Há outros intervalos silenciosos em 21:06,923–21:07,604,
21:07,686–21:07,882 e 22:18,243–22:18,706. Silêncio por si só não demonstra
qual som faltou nem mede atraso entre uma ação e o áudio.

OBS registrou cinco quadros com atraso de renderização e cinco omitidos pelo
encoder em cerca de 99 mil quadros, aproximadamente 0,005% em cada métrica.
Junto da captura direta enviada pelo usuário, isso enfraquece a hipótese de
que OBS seja a origem principal das duplicações. Gravar a 60 FPS não faz o
runtime de 18,5 FPS produzir 60 imagens diferentes por segundo.

Não foi medido atraso audiovisual de conteúdo. A diferença de 31 ms entre
as durações finais das trilhas de vídeo/áudio não é essa medição. O aviso de
atraso do áudio do desktop no log de OBS ocorreu depois do fim da gravação,
com essa fonte silenciada, e não diagnostica o áudio gravado do jogo.

O contador de 1.403 quadros quase pretos inclui transições. A varredura de
vídeo em `analysis/blackdetect-340-end.log` confirma intervalos escuros, mas
vários coincidem com conclusão da missão, seleção de fase e Now Loading.
Não devem ser contabilizados todos como flashes defeituosos.

## Próxima captura para fechar as causas

1. Durante salto/dash/tiro, registrar 8–12 apresentações consecutivas com os
   dois bancos antes/depois do apagamento e do desenho, ordem de escritas
   TVMR/FBCR/PTMR/EWDR/EWLR/EWRR, lista VDP1 e bytes das texturas usadas.
   Pixel antigo já no banco e ausente da lista atual confirma persistência.
2. No pilar, registrar o pattern name, seu bit especial, SFPRMD, prioridade
   calculada do plano e código de prioridade do sprite no pixel de conflito.
3. Correlacionar cada ação sonora com o pacote solicitado ao driver e uma
   referência sincronizada. A execução de sequências/vozes é trabalho
   necessário separado dos ajustes da fila PCM.

Esta rodada produziu diagnóstico e evidência; não alterou o renderer ou o SCSP.

## Correção posterior: limpeza/troca VDP1

Após a priorização pelo usuário, foi corrigido o ciclo do VDP1. O scheduler
conclui erase/change em VBlank-OUT, depois de o handler IN poder solicitar
VBE e antes de o handler OUT desligá-lo. FBCR=3 arma uma troca por escrita;
desenhar não solicita outra troca. PTMR=2 só dispara após uma troca efetiva,
no novo banco de desenho. A limpeza continua respeitando o retângulo EW;
não foi acrescentada uma limpeza indiscriminada por sprite.

Dois replays CPU chegaram a 172M blocos, com saída zero e `step-limit`, mesma
sequência de controle e captura armada em 168M. O trace real mostra TVMR=8 e
FBCR=3 no passo 168.000.006, seguido de TVMR=0 no passo 168.002.125. O caminho
antigo apresentava antes desse pulso e o perdia. Na captura corrigida, o
banco liberado tem zero pixels não vazios, em vez dos 34.129 do caso anterior;
as poses antigas visíveis na comparação desapareceram. As sequências de
quadro diferem (53.015/53.013) devido à mudança de fase de apresentação.

Capturas e manifests:
`build/artifacts/vdp1-before-20261027-run2/`,
`build/artifacts/vdp1-after-20261027-final/`, e JSONs homônimos em `build/tmp/`.
Isso valida esse caso e as regressões de bancos/limpeza; não prova que toda
explosão deformada ou sobreposição do vídeo esteja corrigida. A leitura de
prioridade especial também foi corrigida, mas precisa de replay visual para
confirmar os pontos de oclusão das novas imagens. As capturas F8 posteriores
identificaram outra origem para os pixels coloridos durante o movimento:
interceptação de RAM pelo serviço AIP, descrita abaixo.

Também foram implementados o [debugger de captura ao vivo](LIVE-DEBUGGER.md)
e a [execução isolada do driver SCSP](SCSP-DRIVER-PROBE.md), para fechar os
próximos casos com dados sincronizados.

## Captura F8 de 30/10: causa dos riscos e áudio

As 17 marcações F8 da sessão `build/sessions/x4-red-trail-live-20261030-01/`
confirmaram que as barras coloridas já estavam nos dados de textura antes da
composição. A origem não era o framebuffer nem a prioridade de camadas: o
`BootRomServices::read()` continuava sintetizando leituras da palavra AIP
`0x0600318C` durante o gameplay. O jogo reutiliza essa área dentro do
buffer de sprites iniciado em `0x06003000`; uma cópia LZ lia o contador falso
como dado e o propagava para a textura.

A primeira correção desligava a interceptação somente em `SYS_EXECDMP`.
O probe, que marcava explicitamente esse serviço, passou: no job da captura 2
da sessão 01, as divergências caíram de 329 para zero nos 1.920 bytes
exportados. Entretanto, a sessão 02, iniciada com esse executável, voltou a
registrar os riscos. Seus primeiros POST contêm WRAM sincronizada:

- `capture0001/post_0008_ram_high.bin`: origem comprimida `0x060BC5E2`,
  2.944 bytes decodificados e 40 bytes divergentes; palavras zero viraram
  `0x0571`. A textura VDP1 coincide com a WRAM corrompida.
- `capture0002/post_0008_ram_high.bin`: origem `0x060BD3CA`, 2.688 bytes
  decodificados e 92 bytes divergentes, incluindo palavras falsas `0x0584`.
  A textura VDP1 também coincide com a WRAM.
- `capture0013/post_0008_ram_high.bin`: origem `0x060BC5E2`, 40 bytes
  divergentes com o valor `0x065F`. Nesse instante, a VRAM ainda contém outra
  pose; a diferença entre as poses não foi contada como corrupção de DMA.

Esses arquivos estão em
`build/sessions/x4-red-trail-live-20261030-02/session-1793379341323-0/`.
O boot curto com trace esclareceu o motivo da primeira correção incompleta:
a AIP do X4 salta diretamente de `0x06002F56` para a entrada do programa
`0x06010000`, sem chamar `SYS_EXECDMP`. O trace em
`build/tmp/x4-aip-handoff-trace-1m-20261030.log` alcança essa entrada e não
registra nenhuma chamada ao serviço.

O lifecycle agora possui `BootRomServices::completeAipHandoff()`, chamado
pelo runner antes de executar `expectedEntry`. O caminho `SYS_EXECDMP`
também chama essa API. O probe usa a mesma API para declarar que o snapshot
já pertence ao gameplay. Reexecutando os jobs das capturas 1, 2 e 13 da
sessão 02, tanto o interpretador quanto o JIT produziram zero divergências
nos 1.920 bytes exportados, contra a decodificação independente. Foram
4.078 blocos nas capturas 1/13 e 3.874 na captura 2. A correção não altera o
renderizador, o ciclo de limpeza do VDP1 nem a oclusão VDP2.

O executável alternativo `saturn-sh2-handoff-fixed.exe` foi compilado em
Release para preservar a sessão antiga ainda aberta. A validação visual ao
vivo após reiniciar com a correção completa ainda falta; os probes não
substituem essa confirmação.

A sessão original iniciou com `scsp_driver=0` e `PCM HLE`; seus contadores
registraram zero key-ons do driver, compatível com a falta de vozes e efeitos
por sequência. Os launchers `launch_x4_stage.ps1` e `launch_x4_debug.ps1`
agora usam o driver SCSP 68000 por padrão. `-HleAudio` seleciona o caminho
PCM simplificado. A saída audível dos efeitos específicos ainda precisa ser
confirmada na nova sessão ao vivo.

O aviso `capture record/size limit reached` veio do debugger F8, que limita
cada captura a 48 registros e descarta fases intermediárias excedentes. A
sessão relatou 305 snapshots de diagnóstico descartados; isso não descarta
quadros do jogo nem amostras de áudio. O debugger preservou 17 capturas; o
warning foi acionado pelo limite de registros de uma captura, antes do limite
global de dados da sessão.
