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

## VDP2: prioridade especial ausente

`mergeLayer()` usa uma prioridade fixa por plano em `video.cpp:1488–1511`.
A composição não consulta SFPRMD (`0x0EA`) e a decodificação do tile em
`video.cpp:1970–1990` não propaga seu bit de prioridade especial.

O [manual de prioridade especial do VDP2](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p11_20.htm)
define que o modo 1 obtém o bit menos significativo da prioridade a partir do
pattern name de cada tile. A captura anterior de frame 53.015 tem SFPRMD
`0x0015`, ativando esse modo para NBG0/1/2. NBG0 tem prioridade base 4 e os
comandos do X examinados selecionam prioridade 4. Um tile com bit especial 1
teria prioridade 5 e passaria à frente do X. O compositor atual perde essa
distinção. Falta verificar o pattern name do pilar no ponto de sobreposição.

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
explosão deformada ou sobreposição do vídeo esteja corrigida. A prioridade
especial VDP2 permanece pendente.

Também foram implementados o [debugger de captura ao vivo](LIVE-DEBUGGER.md)
e a [execução isolada do driver SCSP](SCSP-DRIVER-PROBE.md), para fechar os
próximos casos com dados sincronizados.
