# Captura de diagnóstico durante a jogatina

O debugger registra evidências sem abrir menus ou parar a execução com
breakpoints. A coleta é opcional; a serialização dos dumps ocorre em uma
thread separada. Copiar o estado ainda tem custo de CPU e memória, portanto
não se deve tratar uma sessão instrumentada como benchmark de desempenho.

## Iniciar e marcar uma falha

Na raiz do projeto, após compilar `saturn-sh2` em Release:

```powershell
.\tools\diagnostics\launch_x4_debug.ps1
```

O launcher usa renderer CPU, VBlank de 14.000 blocos e a rota de menus já
verificada. O controle passa ao jogador em 160M blocos. Pressione **F8** quando
aparecer um defeito. O título da janela indica esse atalho quando a coleta
está habilitada. A pasta real da sessão aparece no console.

O agente também pode pedir uma captura sem tirar o foco do jogo:

```powershell
.\tools\diagnostics\request_x4_debug_capture.ps1 `
  -SessionDirectory H:\projetos\saturnbox\build\sessions\x4-debug-EXEMPLO `
  -Reason 'X duplicado durante salto e tiro'
```

O helper aceita a pasta do launcher, resolvendo `latest-session.txt`, ou a
subpasta real `session-...`. Ele publica texto em `capture.request`; esse
conteúdo nunca é executado como comando. Um pedido pendente não é
sobrescrito. Se uma captura estiver em andamento, o próximo pedido aguarda.
O runtime consulta a caixa a cada 250 ms, aproximadamente, enquanto apresenta
quadros; não há garantia de resposta durante um travamento do runtime.

Para outras rotas, use `boot` ou `launch` com `--debug-session <pasta>`.
Sem essa opção, o recorder e seus snapshots não são ativados.

## Conteúdo salvo

Cada execução cria uma subpasta única. `status.txt` informa erros, perdas e
limites; `telemetry.csv` registra metadados leves aproximadamente uma vez
por segundo. Cada marcação cria `capture0001`, `capture0002`, etc., com:

- Até oito quadros **PRE**, anteriores ao pedido, e oito **POST**, posteriores.
  São quadros, não segundos; a duração depende do FPS e da disponibilidade do
  histórico no início da sessão.
- Imagem composta PPM e ambos os framebuffers VDP1 em formato raw: 512×256,
  palavras de 16 bits em big-endian. A resolução da imagem composta é a saída
  ativa do runtime.
- Nos POST e nas fases instrumentadas: VRAM VDP1/VDP2, CRAM e registradores.
- Estados `before-erase`, `after-erase`, `after-swap`, `before-draw` e
  `after-draw` quando esses eventos ocorrem durante a janela de captura.
- Histórico de comandos percorridos e um ring independente de 4.096 escritas
  de registradores, para preservar pulsos TVMR/FBCR apesar dos uploads de
  textura. O histórico de escrita da textura do jogador continua separado.
- No primeiro POST: WRAM baixa/alta, RAM SCSP e slots de estado da CPU.
- Horário UTC em milissegundos, tempo desde o início da coleta, bloco/passo,
  pad do port 1, posição bruta de X e contadores de áudio/frontend.

As imagens compostas dos eventos intermediários podem pertencer à composição
anterior. Consulte `composition_dirty`, as flags de banco e os dumps raw para
analisar erase/swap/draw; não interprete cada PPM de fase como uma nova imagem
que chegou à tela. A interpretação 16.16 de `player_x` permanece inferida.

Os horários ajudam a localizar o trecho no OBS, mas não constituem uma
medição de sincronismo audiovisual. Este debugger registra **estado e
contadores** do áudio, sem gravar PCM. Continue usando OBS para ouvir e
comparar os efeitos. Os contadores de comandos SCSP podem incluir
reobservações de pacotes pendentes.

## Limites e encerramento

A fila de escrita, o histórico e o tamanho estimado dos pacotes são
limitados. A configuração atual admite oito PRE, oito POST, até 48 registros
por marcação, até 64 marcações e orçamento estimado de 4 GiB por sessão. O
limite das fases reserva espaço para os POST. A fila admite até 64 itens e
256 MiB estimados; o ring PRE admite até 128 MiB. Sob pressão, registros podem
ser descartados; `dropped_snapshots` e `error` deixam isso explícito.

Ao fechar normalmente o jogo, o worker termina de escrever a fila. Um
encerramento forçado pode perder itens que ainda estavam em memória. Sessões
anteriores são preservadas. O console também informa falhas de gravação.

Validação inicial: testes de snapshot sem recomposição, retenção do pulso VBE,
ordem de fases, PRE/POST, pedidos pendentes, limites, erros e preservação de
sessões. Um teste integrado executou 8M blocos SH-2 enquanto recebeu um pedido
externo, salvando oito PRE/oito POST e os dumps de memória, sem perdas.
O atalho F8 está conectado à janela; o teste integrado automatizado usou o
pedido por arquivo.
