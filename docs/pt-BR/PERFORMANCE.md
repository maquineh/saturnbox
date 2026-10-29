# Desempenho do Rockman X4

O alvo da execução interativa é 60 apresentações por segundo. O benchmark
mantém o período de 14.000 blocos por VBlank e executa a emulação completa,
incluindo o 68000 e os slots SCSP. O modo sem janela mede capacidade de
processamento; ele não mede a taxa de apresentação do monitor.

## Resultado medido em 2026-10-29

Máquina: Intel Core i7-9700 e NVIDIA RTX 3080, MSVC Release, SCSP real,
período de 14.000 blocos. As medições sem janela são de processamento; a
última linha é a média de apresentação informada pelo frontend.

| Configuração | Rota | Taxa medida |
| --- | --- | --- |
| Referência CPU, 2.443 blocos estáticos | 25M blocos, sem janela | 24,13 VBlank/s |
| CPU com AOT treinado na rota inicial | 25M blocos, sem janela | 70,01 VBlank/s |
| Vulkan com AOT treinado na rota inicial | 25M blocos, sem janela | 76,03 VBlank/s |
| Vulkan, 8.968 blocos estáticos, janela e áudio | 172M blocos, 12.285 apresentações | 59,59 FPS |

A última execução ficou em 16,76 ms de intervalo médio, p95 abaixo de 19 ms
na resolução do histograma e máximo de 399,59 ms. Ela registrou 9 filas de
áudio vazias, zero esperas por buffer, zero erros de escrita e zero blocos
truncados. Portanto o alvo de aproximadamente 60 FPS foi alcançado nessa
rota, mas os picos e a recuperação de áudio ainda não estão eliminados.
Os eventos ficam registrados por índice de apresentação; não se deve
interpretar somente a média de FPS como reprodução de áudio perfeita.

Todos os 178.217.944 blocos executados pelos dois SH-2 nessa rota foram
estáticos, sem fallback ao JIT. O cache do resolver teve 6.217.922 hits.
O conjunto final passou em 185 testes, incluindo a equivalência do driver
com e sem dispositivo em tempo real, invalidação de código, aliases,
fronteiras de página, composição e captura de diagnóstico.

Artefatos locais: `build/perf/isolated-final/results.json`, `run-1.log` e
`audio-1.wav`. O executável validado foi copiado para o caminho padrão
`build/tools/saturn-sh2/Release/saturn-sh2.exe`; SHA-256
`C22A2894A7148990A0A73048BEF3A2F4434AF67D38EED3F1EB4CC63ABFA234DD`.
O cache local do build aponta para os módulos em `build/perf/aot-trained172`.

## Compilar e medir

```powershell
cmake -S . -B build -DSATURNBOX_MUSASHI_68K=ON -DSATURNBOX_IPO=ON
cmake --build build --config Release --target saturn-sh2 saturnbox-tests --parallel
.\build\tests\Release\saturnbox-tests.exe
.\tools\diagnostics\benchmark_x4.ps1 -Runs 3
.\tools\diagnostics\benchmark_x4.ps1 -Profile
.\tools\diagnostics\benchmark_x4.ps1 -Realtime -Steps 172000000
.\tools\diagnostics\benchmark_x4.ps1 -Renderer vulkan
.\tools\diagnostics\benchmark_x4.ps1 -Renderer vulkan -Realtime -Steps 172000000 -CaptureAudio
```

São necessários os arquivos de boot em `build/rockman` e a imagem do disco.
`-DiscImage` e `-ExecutablePath` permitem selecionar outros caminhos.
O IPO é opcional, restrito a Release e aos alvos C++ do projeto. O gerador
Musashi e o núcleo externo preservam suas opções de compilação separadas.

Cada execução cria uma pasta exclusiva em `build/perf`, com log, argumentos,
SHA-256 do executável e `results.json`. O modo interativo também registra
o resumo do frontend: FPS de apresentação, intervalos de quadro e problemas
de reprodução de áudio. O tempo externo inclui inicialização e relatórios.
Compare executáveis com a mesma rota, opções e carga de trabalho no computador.

`-Capture` salva o áudio inteiro e uma captura perto do final, com bancos e
memória para comparação de conteúdo. Essa opção acrescenta instrumentação,
cópias e escrita em disco: seus resultados não são diretamente comparáveis
com os de uma execução sem captura. Ela é útil para verificar que uma
otimização preserva os dados produzidos.

## Cobertura de compilação estática

O cache do JIT acelera blocos estáveis, mas o C++ pré-compilado permite ao
compilador otimizar o corpo inteiro. Para incluir o código efetivamente
percorrido pelo jogo, exporte um módulo após uma rota determinística:

```powershell
.\tools\diagnostics\benchmark_x4.ps1 -ExportAot -OutputDirectory build/perf/training
.\tools\diagnostics\merge_x4_aot.ps1 `
    -Modules build/rockman/rockman_static.cpp,build/perf/training/module-1.cpp `
    -OutputDirectory build/perf/static-trained
cmake -S . -B build -DSATURNBOX_STATIC_MODULE_SOURCE= `
    -DSATURNBOX_STATIC_MODULE_DIR=H:/projetos/saturnbox/build/perf/static-trained
cmake --build build --config Release --target saturn-sh2 --parallel
```

O caminho absoluto do exemplo deve apontar para a pasta gerada na sua cópia.
`-Steps 172000000 -ExportAot` percorre também a entrada na fase. Exports
separados podem ser combinados na ordem desejada. O agregador conserva
assinaturas diferentes no mesmo endereço, remove duplicatas e registra hashes
no manifesto. O runtime só seleciona uma função estática quando a assinatura
do bloco vivo coincide; código novo continua usando o JIT.

O export final de um módulo contém a versão corrente de cada endereço.
Conserve exports de diferentes pontos se overlays reutilizarem um endereço.
Os fontes gerados contêm código derivado do jogo e ficam em `build`, sem
entrar no repositório. `-ExportAot` também acrescenta tempo ao relatório final.

## Mudanças no caminho quente

* O JIT consulta a revisão do bloco antes de reconstruir sua assinatura.
  Reset e append continuam invalidando o código compilado.
* A resolução de blocos usa o epoch de escrita da RAM para evitar consultas
  repetidas de geração de página. Hooks que escrevem memória continuam
  provocando a revalidação antes da execução.
  O resolver compartilhado pelo segundo SH-2 também valida identidade,
  revisão e as páginas do código antes de reutilizar um bloco, evitando
  reconstruir snapshots e decodificar as mesmas instruções em cada iteração.
* O contexto de execução é reutilizado entre blocos. Seu snapshot de
  registradores é limpo antes de cada publicação e preenchido quando solicitado.
* Leituras de RAM usam um cache de 64 páginas com tags. Hooks e aliases são
  consultados antes do acesso; cópias e movimentos do estado limpam o cache
  de ponteiros, e a alocação de uma página atualiza entradas ausentes.
* O VDP2 reutiliza buffers intermediários, decodifica cada nome de tile uma
  vez por trecho de linha e evita camadas de prioridade zero fora do diagnóstico.
  A resolução de cor final ocorre uma vez, após a composição dos sprites.
  Camadas sem janelas habilitadas evitam o predicado de janela por pixel.
* A captura de origem dos sprites delega a necessidade de snapshot ao
  interceptor seguinte, pois só usa os campos escalares do contexto.

## Áudio durante a execução interativa

Com o driver real, abrir o dispositivo de áudio mantém o relógio por blocos:
o 68000 continua recebendo 256 ciclos por amostra e o quadro de 60 Hz contém
735 amostras por canal. O relógio do Windows permanece uma opção apenas do HLE
legado. Isso impede que uma janela lenta faça o driver avançar à frente dos
comandos e transferências do SH-2.

O frontend mantém backpressure para preservar PCM. FPS acompanhado de cortes,
repetição de música ou crescimento da fila de áudio não atende ao alvo.
Compare o WAV gerado, mas confira também underruns, esperas e intervalos de
apresentação: igualdade do WAV sozinha não prova ausência de atraso no dispositivo.
`-CaptureAudio` grava o WAV sem o despejo pesado de memória de `-Capture`.
O gravador reserva antecipadamente espaço para até cinco minutos para evitar
cópias grandes do vetor de PCM no meio da reprodução.

Uma fila vazia continua sendo contada e seus primeiros 128 índices de quadro
aparecem no relatório. O frontend não pausa/reinicia o dispositivo nesse caso:
entrega o próximo bloco diretamente. Essa recuperação segue o comportamento
documentado do [waveOutWrite](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutwrite).

## Interpretar o perfil

`--profile` imprime os custos de SCSP por bloco, mailbox, scheduler MMIO,
SCSP no VBlank e vídeo no VBlank OUT. Os três caminhos frequentes medem uma
chamada a cada 1009 e estimam o total pela média das amostras; os caminhos de
VBlank medem todas as chamadas. A amostragem e o relógio acrescentam custo,
portanto essas estimativas servem para localizar gargalos, não para provar
pequenas diferenças entre builds.

O custo de vídeo inclui os consumidores síncronos do quadro. Com janela e
sincronização ele inclui esperas; com captura inclui o diagnóstico. Use o
perfil sem janela para investigar o custo de processamento. O restante do
tempo total inclui SH-2, resolução, callbacks e acessos à memória.

## Evidência de preservação

O primeiro lote foi comparado com a build anterior por 172 milhões de blocos,
até a fase inicial. A captura no bloco 171 milhões e o WAV inteiro produziram
os mesmos SHA-256:

* Imagem PPM: `3C636F82794C9D6A71C5E8CD6A61C22BF10AAF4C24FA7470D674F778A815CAF7`.
* Áudio WAV: `D4EE209E5A3E94C1C508E77265D2867002ADE757A14E012B95CF5793BFC852CB`.

Os logs funcionais também coincidiram, exceto pelos caminhos de saída. Isso
verifica preservação nessa rota; não valida a correção dos glitches de
vídeo ou dos efeitos sonoros já existentes.

A mesma imagem foi preservada no backend Vulkan com janela. Depois da
correção do relógio SCSP e das demais otimizações, o WAV da medição isolada
final também manteve o SHA-256 acima. A captura de memória foi desativada
nessa última medição para não introduzir pausas de escrita durante o jogo.
