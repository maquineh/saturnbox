# Rockman X4 — baseline de análise

Este documento registra um corpus local de validação. Nenhum arquivo do jogo é
versionado no repositório.

## Imagem identificada

```text
Título:             ROCKMAN X4
Produto:            T-1221G V1.002
Data:               1997-06-17
Regiões:            Japão e Taiwan
IP.BIN:             0x06002000, 0x1800 bytes
AIP entry:          0x06002E40
Primeiro arquivo:   0.BIN, 415320 bytes
Load address:       0x06009000
Entry resolvido:    0x06010000
```

O entrypoint do executável não é o endereço de carga. O AIP resolve uma
transferência para `0x06010000`, deixando `0x7000` bytes entre o início de
`0.BIN` e o primeiro código analisado.

## Reprodução

```sh
saturn-disc inspect <imagem.mdf>
saturn-disc extract-aip <imagem.mdf> --output aip.bin
saturn-disc extract <imagem.mdf> 0.BIN --output 0.BIN

saturn-sh2 analyze aip.bin --base 0x06002E40 --entry 0x06002E40
saturn-sh2 analyze 0.BIN --base 0x06009000 --entry 0x06010000
```

## Baseline atual

```text
AIP:
  basic blocks:                  64
  decoded instructions:        282
  SIR-liftable non-control: 227/227 (100.0%)
  concrete SIR blocks:        64/64
  concrete SIR operations:      754
  unsupported sites:             0
  unresolved indirect transfers: 6

0.BIN:
  basic blocks:                1883
  decoded instructions:      10859
  SIR-liftable non-control: 9252/9252 (100.0%)
  concrete SIR blocks:      1883/1883
  concrete SIR operations:    32172
  unsupported sites:             0
  unresolved indirect transfers: 77
```

Os opcodes acrescentados a partir desse corpus incluem modos `MOV` indexados e
com deslocamento, `SHLL2`, extensões de sinal/zero, lógica imediata,
`LDC/LDS/STC/STS` e divisão. A próxima expansão deve atacar análise de valores
e tabelas de função, não aumentar o decoder sem evidência do corpus.

O lifter cobre todas as instruções não terminadoras atualmente alcançadas. As
operações que leem ou escrevem `T/M/Q` usam primitivas SIR explícitas para
extração de bits, carry/borrow, overflow e o passo de divisão SH-2; nenhuma flag
é aproximada ou descartada.

O comando `saturn-sh2 run` executa esse módulo em blocos com `--steps` como
limite. Os 77 transfers indiretos de `0.BIN` permanecem desconhecidos na análise
estática, mas passam a ser resolvidos pelo valor runtime do registrador e baixados
sob demanda se o destino estiver na imagem.

## Harness de boot

O início do M2 permite combinar as regiões do AIP/IP e de `0.BIN`:

```sh
saturn-sh2 boot aip.bin --aip-base 0x06002E40 \
  --ip ip.bin --ip-base 0x06002000 \
  --program 0.BIN --program-base 0x06009000 \
  --entry 0x06002E40 --expected-entry 0x06010000 --steps 1000000
```

O comando observa a entrada de `0.BIN`. `BootRomServices` instala os ponteiros
`SYS_EXECDMP` (`0x0600026C`), `SYS_CDINIT1/2` (`0x060002DC`/`0x0600029C`), os
despachos de interrupção em `0x06000300`, `0x06000304`, `0x06000310` e
`0x06000314`, e os serviços de clock, semáforo e máscara em `0x06000320`,
`0x06000330`, `0x06000334`, `0x06000340` e `0x06000344`. Os serviços
retornáveis usam blocos SIR explícitos. `SYS_SETUINT`/`SYS_GETUINT` atualizam
a tabela SCU em `0x06000900`, usando `0x06000610` para o handler nulo;
`SYS_CHGSCUIM` mantém a cópia em `0x06000348` e espelha a operação nos
registradores SCU (`0x25FE00A0`, `0x25FE00A4` e, quando aplicável,
`0x25FE00A8`). O alias de cache `0x25xxxxxx` é normalizado para a janela
`0x05xxxxxx` dos runtimes CD/VDP. A palavra de sincronização em `0x0600318C`
também é modelada para que o executor não fique preso no polling de boot/CD em
uma execução single-threaded.

Com `--steps 1000000`, o Rockman X4 alcança `0x06010000`, resolve o fluxo que
contém `CMP/STR` (`0x264C`) e termina em `status: sleeping` no stub de
`SYS_EXECDMP`. O runtime CD/VDP já está ligado e os aliases são observados no
trace. A opção `--continue-after-execdmp` entra no CD multiplayer oficial e
termina no polling de HIRQ/SCU; o scheduler opcional (`--vblank-blocks N`)
agora avança a apresentação em cada VBlank. A sequência de comando `0x61` do CD
pode ser drenada para um destino SH-2 com `--resource <iso-path>
--resource-base <address>`, inclusive nas VRAMs VDP1/VDP2, e o primeiro
`VideoFrame` compõe BACK/NBG0/NBG1 antes de sobrepor a tabela VDP1.
O runtime SCU registra a configuração real do upload: no recorte atual o
canal D0 recebe `D0R=0x06060594`, `D0W=0x25C00000`, `D0C=832` bytes,
`D0AD=0x101` (`R4/W2`) e `D0MD=7`; a transferência completa 208 words para
o VRAM VDP1 e mantém os endereços visíveis, como definido pelos bits de update.
O compositor VDP2 também aceita o caminho cell: mapas A/B/C/D, PNT de uma ou
duas words, células 8x8 paletizadas/RGB e transparência.

## Trace contextual M2

O boot com `--trace --pad1 start,a` termina após 372.445 blocos. A entrada do
programa é alcançada, 1.023 blocos são baixados sob demanda e 22 chamadas de
serviço são registradas. A chamada final é:

```text
SYS_EXECDMP
  caller: 0x0601CED4
  step:   372444
  R0:     0xFFFFFFE9
  SP:     0x060097C4
  PR:     0x0601D096
```

O trecho em `0x0601CED4` é um trampolim que lê o ponteiro de `0x0600026C` e
salta para ele. Antes dessa fronteira, os únicos acessos MMIO observados são os
comandos SMPC `SNDOFF` (`0x07`) e `SNDON` (`0x06`); não ocorre `INTBACK`.
O runtime de CD/vídeo já está conectado ao MMIO: `--disc` injeta uma imagem raw
no FIFO/na máquina de comandos do CD Block, com leitura ISO-9660 por caminho,
e VDP1/VDP2 mantêm memória, registradores, tabela de comandos e frames RGB555;
`INTBACK` é reamostrado no VBlank quando armado.
No recorte medido, a continuação opcional registra 311 acessos ao CD e 123.888
ao VDP2, mas nenhum `PTMR=1`; com o período padrão (`--vblank-blocks 0`) o
contador de frames permanece zero. Com `--vblank-blocks 10000`, o handler de
VBlank monopoliza o recorte e a transferência não alcança o handoff. Com
`--vblank-blocks 400000`, o handoff é alcançado, um primeiro `VideoFrame` é
publicado, a conclusão de DMA0 é entregue pelo vetor `0x4B` e o fluxo atravessa
`0x060303CC`. O polling em `0x06010722` aguardava o mailbox de sincronização
em `0x0605E796`/`0x0605E798`; o runtime agora agenda o produtor do segundo
processador, incrementa `0x0605E790` e publica as duas flags quando os pedidos
em `0x0605E792`/`0x0605E79A` aparecem. A rotina também acessa TVMR/FBCR em
`0x25D00000`/`0x25D00002`, portanto o rótulo histórico “SCSP/68K” não deve ser
confundido com síntese SCSP: o caminho é uma sincronização de framebuffer/VDP1
com mailbox compartilhado. Ainda não há comandos VDP1 reais no corpus (`PTMR=1`).
O runtime já possui a semântica de término de desenho: quando um caminho futuro
escrever `PTMR=1`, o scheduler publicará o fator SCU de sprite draw end (bit 13,
vetor `0x4D`) no limite de bloco seguinte.

O estado mais recente foi medido com a imagem raw Mode 1/2448 completa
(244.711 setores) e `--vblank-blocks 10000`. O fluxo de CD agora permanece
estável durante a carga: a sequência `PLAY` → `0x51` → `0x52`/`0x53` →
`0x61`/`0x62` chega ao DMA e 200 setores são consumidos sem reinicializar a
seleção do CD. O suporte de DMA interno SH-2 também conclui o canal 0, com
832 bytes transferidos para a VRAM do VDP1.

O avanço de boot exigiu três compatibilidades que já estão instaladas: a
divisão SH-2 (`DIV0S/DIV0U/DIV1`), `BUPInit` seguindo a tabela de BIOS do
Yabause e `MULU.W/MULS.W`. Com elas o jogo liga o display e o compositor:
`TVMD=0x8010`, `BGON=1` e `CHCTLA=0x42`; o scheduler produz VBlank e o frontend
recebe frames. Ainda assim, os frames permanecem totalmente pretos. Ao fim de
5 milhões de blocos, a lista VDP1 copiada pelo DMA contém apenas o cabeçalho
`0x00090000`, o framebuffer VDP1 continua zerado e o bitmap NBG0 começa em
zero embora existam dados em outra região da VRAM VDP2.

O fluxo de SH-2 então fica no bloco de espera `0x06015D48`, que consulta o
byte de evento em `0x060519CA` e retorna ao dispatcher `0x060303D8`/
`0x06030854`. Portanto o limite atual não é mais a leitura de setores nem a
cadência do frontend: é a preparação, pelo runtime do jogo, dos dados de
desenho/eventos que deveriam alimentar VDP1/VDP2. A próxima investigação deve
seguir esse dispatcher e o callback em `0x06034D72`, preservando os endereços
reais de `MPOFN`/NBG0 em vez de introduzir um deslocamento artificial.

O trace focado posterior mostrou que `0x06034D72` não está ausente: ele é
chamado 664 vezes em 1,5 milhão de blocos, sempre a partir de `0x06011AF8`, e
continua por `0x06034D98`. Essa rotina valida/configura a transferência de 832
bytes de `0x06060594` para `0x25C00000`; ela não publica o evento do carregador.
No mesmo recorte, `0x060519CA` sofre 194.902 leituras e somente três escritas,
todas com zero: a limpeza ampla em `0x06010680`, a inicialização em
`0x06015B4A` e a preparação imediata da espera em `0x06015D46`. Portanto o
próximo alvo é o caminho de conclusão assíncrona iniciado antes de
`0x06015D42`, que deveria escrever um valor não-zero nesse byte; não é mais
necessário investigar `0x06034D72` como possível callback ausente.

A instrumentação desse caminho confirmou que a tarefa GFS termina normalmente:
`0x06039C22` chama o método de atualização, `0x0603A018` resolve o objeto em
`0x0607C038` e o estado em `+8` muda para `1` antes de `0x06015D42`. A rotina
então limpa `0x060519CA` e aguarda um evento independente. A referência estática
desse byte revelou o produtor em `0x060166AC`, instalado por `SYS_SETUINT` no
vetor `0x43`. Os literais próximos são `0x25FE0090` e `0x25FE0098`: trata-se do
comparador e do modo do Timer 0 da SCU, não de DSP-end.

O runtime agora preserva esses dois registradores e gera o fator SCU bit 3 no
HBlank da linha configurada. Em um boot real de 2 milhões de blocos, o jogo
limpa a flag no passo 1.304.201, o callback `0x060166AC` grava `1` no passo
1.612.729 e o polling sai três blocos depois. A execução continua por novos
ciclos GFS/CD, liga `TVMD=0x8010`/`BGON=1` e o frame composto passa a conter
pixels não pretos. O próximo recorte deve seguir os novos ciclos de carga e a
produção do primeiro quadro útil, não voltar ao polling já resolvido.

Em 5 milhões de blocos o Timer 0 entrega 11 callbacks e `0x06015D48` não volta
a acumular hits depois da saída, confirmando que o bloqueio foi removido. O
compositor, porém, permanece em 38 pixels não pretos: a tarefa ativa em
`0x0607C038` volta ao estado `0`, enquanto o método de atualização apontado por
`+0x1B4` é `0x0603A646`. Esse método foi acrescentado ao trace assíncrono e é o
próximo alvo para explicar por que os dados já carregados ainda não produzem a
primeira tela útil.

O trace interno de `0x0603A646` mostrou que a rotina em si continua válida. Ela
consulta o anel embutido em `0x0607C038`, inicia transferências somente quando
há pelo menos `0x800` bytes e avança o consumidor por `0x0603EE10`. Foram 106
avanços do consumidor, mas somente quatro publicações do produtor em
`0x0603EED6`. Depois delas, os cursores ficam em `+84=0x46800` e
`+88=0x18B0C`, com capacidade `+112=0x2E000`; a disponibilidade calculada é
apenas `0x30C`. Por isso `0x0603A646` sai pelo caminho sem transferência, não
por falha do polling assíncrono.

Também foi corrigida uma limitação independente no PLAY do CD HLE. O jogo pede
954 setores, mas o runtime limitava todo o intervalo aos 200 slots do buffer
retido e publicava PEND prematuramente. Agora o intervalo é limitado apenas
pelo fim da imagem; quando os 200 slots ficam cheios, o produtor aplica
backpressure e retoma depois de um `0x62`. O teste específico cobre um PLAY de
250 setores atravessando essa fronteira. Na imagem real o cursor passou de
`200/200` para `247/954`, confirmando a correção, embora o jogo ainda pare de
emitir comandos depois de 126 pares `0x61`/`0x62`.

Os quatro primeiros lotes do anel vieram de `0x0603FFEA` e `0x0603EF6C`. Uma
linha do tempo causal dos blocos reais `0x06040724`, `0x0604073A`,
`0x06040744`, `0x0604074C` e `0x06040762` corrigiu a interpretação inicial: o
dispatcher migra, sim, para `0x060401D4`. Depois de publicar o descritor 3, ele
chega ao descritor 4 (`+8=0x80000004`), mas `0x060401D4` retorna `-1` porque a
transferência anterior ainda aparece ativa em `ring+0x20`.

O poll de conclusão em `0x060409E6` devolvia progresso zero contra o limite 2,
seguia sempre por `0x06040784` e nunca chamava `0x0603FBC8` para limpar o campo
ativo. A origem não era o CD: `0x06040816` lê o free-running counter interno do
SH-2 em `0xFFFFFE12/13`, ainda não modelado. O MMIO agora implementa o FRC de
16 bits e o prescaler `TCR.CKS` de forma determinística, usando aproximadamente
oito ciclos por bloco traduzido. Há um teste dedicado para leitura byte/word,
overflow e divisores phi/8 e phi/32.

Com o FRC ativo, o poll passou de zero para 3, entrou em `0x06040794`/
`0x0604079C` e executou a conclusão. Em 3 milhões de blocos, as publicações do
anel subiram de 4 para 8, o índice do dispatcher avançou de 4 para 8 e o frame
composto passou de 38 para 2.560 pixels não pretos. Isso confirma que o caminho
GFS voltou a progredir sem forçar índices, retornos ou interrupções. O vetor
A-bus `0x50` continua com handler nulo e mascarado, portanto permanece sem
evidência qualquer atalho por interrupção de CD.

O teste prolongado com janela e captura de frames confirmou outro bloqueio.
A execução planejada para 10 milhões foi encerrada pela janela após
8.015.198 blocos observados (o relatório de `frontend-quit` mostra incorretamente
zero em `completed block steps`; os contadores do observer e os passos do trace
preservam o avanço real). Foram apresentados 4.608 quadros. O conteúdo muda até
o quadro 688 e permanece idêntico até o final: apenas faixas no topo, com 2.560
pixels não pretos, sem uma tela reconhecível do jogo.

O GFS publica 18 lotes, o último no passo 5.656.155, e estaciona no descritor
18/139 em `0x0608032C`: `{0x5458C, 0x5628, 0xFFFFFFFF, 1}`. O campo ativo
`ring+0x20` está zero; o bloqueio anterior de conclusão foi superado. Depois da
última transferência CD, concluída por volta de 6.018.271, o anel fica com
`+84=0x82800`, `+88=0x54E7C`, capacidade `0x2E000` e disponibilidade `0x67C`,
abaixo de um setor. O CD continua com 200 setores retidos, cursor `460/954`.

Uma repetição sem frontend, de 6,2 milhões de blocos, com instrumentação do
epílogo `0x0603FFF4`, localizou o retorno `-1` em `0x0603FDC8`. A consulta
`0x060430CC` devolve `0x28A8` livres, comparados em `0x0603FDC4` com
`R13=0x2B14` necessários. Os ponteiros retornados são `0x25A6D758` e
`0x25A7D758`, na RAM de som. Portanto o próximo alvo é o cálculo de espaço
livre desse buffer e a atualização de seus cursores de consumo: a rotina usa
`ring+0x90`, `+0xA0`, `+0x98`, `+0x80` e `+0x88`. A causa da falta de avanço
desses cursores ainda precisa ser demonstrada; não foi aplicada uma liberação
artificial de espaço.

Artefatos: `build/artifacts/gfs-window-10m.txt` (execução interrompida em 8 M),
`build/artifacts/gfs-descriptor18-diagnostic.txt` (6,2 M completos) e
`build/artifacts/gfs-window-10m-frames/frame_004608.png`. As capturas PPM
duplicadas foram removidas, preservando cada mudança e o quadro final.

### Cursor de consumo do buffer de som

A instrumentação de `0x060430CC` resolve o controlador dinamicamente pelo
wrapper e registra mudanças em `ring+0x80..0xAC`. Duas execuções de 6,2 milhões
de blocos reproduziram o bloqueio, sem alterações funcionais no runtime.
O produtor em `0x06043186` incrementou `ring+0x98` até `0xD758` e o ponteiro
`ring+0x88` até `0x25A6D758`. Os campos de consumo `+0x90` e `+0xA0`
permaneceram zero desde a primeira consulta observada.

Para este stream de 8 bits, a consulta calcula:
`livre = ring[0x80] - (ring[0x98] - ((ring[0x90] << 12) + ring[0xA0]))`.
Logo `0x10000 - 0xD758 = 0x28A8`: o cálculo está consistente; faltam
`0x26C` bytes para aceitar os `0x2B14` do próximo lote. Não foi forçado
retorno de sucesso nem liberação artificial do buffer.

O caminho de notificação foi localizado no código original:

- `0x0603EAC4` chama o poll `0x06042B50`.
- O poll compara oito bytes, espaçados de dois em dois, na tabela
  `0x25A000A0 + [0x25A00404]`, com o espelho em `0x0606E608`.
- Uma mudança chama `0x06042AA6`, que filtra streams ativos e condições de
  fila; o caminho `0x06042AFC` incrementa `ring+0x90` e chama `0x06040884`
  para renovar a referência do timer em `ring+0xA4`.

No segundo teste houve 14 entradas no poll, zero entradas no dispatcher
`0x06042AA6` e zero no incremento `0x06042AFC`. A última leitura de status
foi `0x25A007AE = 0`, com `[0x25A00404]=0x700`, identificando a tabela
efetiva em `0x25A007A0`. O contador de 224 leituras inclui tanto a leitura
do deslocamento quanto a dos bytes de status. Isso demonstra ausência de
notificações detectadas, não apenas um cursor final parado.

O serviço SCSP atual limpa o bit de comando em `0x25A004E0`, mas não executa
o driver 68K nem publica o consumo nessa tabela. O próximo trabalho deve
estabelecer o contrato de comando/reprodução/status desse driver e modelar
a publicação a partir do consumo real de amostras. O significado exato dos
bytes e sua cadência ainda precisam ser validados; incrementar o cursor SH-2
diretamente esconderia essa lacuna. Esta etapa não destravou o jogo.

Artefatos: `build/artifacts/gfs-audio-cursor.txt` e
`build/artifacts/gfs-audio-status.txt`. Build Release e CTest passaram.

### Serviço PCM e publicação do status

A captura dos pacotes enviados pelo SH-2 confirmou um PCM Start no passo
1.302.592, em `0x25A00750`:
`850090E0 60000000 00000000 00000000`. A leitura do código que monta o
pacote e do manual Sega identifica o stream 0, estéreo, PCM de 8 bits,
início `0x60000`, 65.536 amostras por canal e pitch zero (44.100 Hz).

O runtime SCSP agora implementa o subconjunto PCM Start/Stop (`0x85/0x86`)
do driver. A fila contém oito pacotes de 16 bytes, na área apontada pelo
longword em sound RAM `0x404`; não depende de endereços do objeto SH-2.
Cada comando PCM consumido tem seu código limpo, evitando reinícios por
reenvio. O mixer lê amostras assinadas, trata canais separados, pitch e wrap,
e publica a posição em unidades de 4.096 amostras em `host+0xA0+2*stream`.
O avanço ocorre mesmo sem frontend de áudio ou com volume zero.

O teste de 6,2 milhões de blocos registrou um único início PCM, duas mudanças
de status e duas entradas no caminho original `0x06042AFC`. O próprio SH-2
atualizou `ring+0x90` para 1 no passo 3.600.290 e para 2 no passo 6.000.290.
O WAV resultante tem 11.025 frames estéreo a 44.100 Hz, com amostras não nulas.
O teste terminou no descritor 15, ainda em avanço/sincronização; isso não deve
ser comparado apenas por número de blocos com a versão sem consumo de áudio.

Limites: trata-se de HLE do contrato PCM, não de execução do 68K ou SCSP
completo. Comandos de sequência, DSP e alteração de parâmetros PCM continuam
sem implementação. A publicação usa o relógio de áudio atual, atualizado por
VBlank; não há geração da interrupção opcional de posição PCM. Uma tentativa
de limpar todos os comandos, inclusive os não implementados, provocou uma
regressão anterior ao vídeo e foi descartada. A versão mantida confirma
somente Start/Stop e preserva o caminho de compatibilidade dos outros comandos.

Validação: 75/75 testes C++ passaram, incluindo PCM 8/16 bits, estéreo, pitch,
wrap, parada, interface relocada, limites de RAM e comando não implementado.
Artefatos: `build/artifacts/gfs-sound-commands.txt` (captura anterior),
`build/artifacts/gfs-pcm-scoped.txt` e `build/artifacts/gfs-pcm-scoped.wav`.
`gfs-pcm-consumption.txt` pertence à tentativa descartada, não à versão final.

A extensão para 14 milhões de blocos (`build/artifacts/gfs-pcm-14m.txt`)
ultrapassou o antigo bloqueio: 21 publicações e descritor 21/139, contra o
antigo estacionamento no 18. Houve um único PCM Start e cinco mudanças de
status, todas atendidas pelo dispatcher original. `ring+0x90=5`,
`ring+0x98=0x1026C` e `ring+0x88=0x25A6026C` demonstram também o wrap do
produtor. O primeiro produtor retornou sucesso no passo 10.429.852.
Ao final, `ring+0x20=1` e o poll devolvia `0x1C` contra o limite `0x1E`:
é uma espera de sincronização observada, ainda não um novo travamento provado.
Os pixels não pretos continuam em 2.560; o avanço do stream não confirma
correção visual. Os testes dessa etapa foram sem janela; o próximo teste
visual prolongado deve verificar a sincronização e localizar a primeira
espera que realmente permaneça estacionada.

Referências: manual Sega, [comandos PCM](https://docs.exodusemulator.com/Archives/SSDDV1R1E/c/c08/hon/p017.htm),
[parâmetros](https://docs.exodusemulator.com/Archives/SSDDV1R1E/c/c08/hon/p021.htm)
e [posição de reprodução](https://docs.exodusemulator.com/Archives/SSDDV1R1E/c/c08/hon/p037.htm).

### Teste prolongado com janela após o serviço PCM

Execução `launch` com a ISO MDF de `H:\Isos`, `--steps 20000000`,
`--vblank-blocks 400000`, `--trace-game`, captura de áudio e
`--frame-dir build/artifacts/gfs-pcm-window-20m-frames --frame-changes-only`.
A janela respondeu durante o teste e a execução encerrou normalmente por
`step-limit`, sem fechamento antecipado. O log abrange aproximadamente
7min28s (21:30:16–21:37:44 de 18/09/2026).

O trace agora emite uma amostra de progresso por milhão de blocos. O filtro
opcional de capturas compara dimensões e pixels, sem suprimir apresentações
na janela nem alterar a execução emulada. Nenhuma correção funcional de
sincronização foi aplicada nesta etapa.

| Blocos | Descritor | Cursor de consumo | Transferência ativa |
| --- | --- | --- | --- |
| 8 milhões | 15 | 2 | não |
| 10 milhões | 17 | 3 | não |
| 14 milhões | 21 | 5 | sim |
| 15 milhões | 24 | 6 | sim |
| 17 milhões | 25 | 6 | não |
| 20 milhões (final) | 26 | 8 | sim |

A espera do descritor 21 foi superada. No encerramento havia 26 publicações,
um único PCM Start e oito mudanças de status atendidas pelo SH-2. O último
poll devolvia `0x1D` contra o limite `0x26`, com `ring+0x20=1`. Isso registra
uma espera de sincronização no limite do teste, não prova um novo travamento.

Foram apresentadas 23.184 imagens, mas houve somente 50 VBlanks e 50 blocos
de áudio: 36.750 amostras por canal a 44.100 Hz, isto é, 0,833 s de áudio.
A origem dessa diferença está explícita no código: `VideoRuntime::writeVdp1Register`
chama `advanceFrame()` em cada disparo manual `PTMR=1`; o frontend chama
`paceFrame()` em cada apresentação, com período de 16.667 us. Já o SCSP
avança no VBlank do scheduler. Portanto, redesenhos manuais fazem a janela
esperar tempo real sem avançar proporcionalmente o relógio de áudio.

O próximo alvo recomendado é separar conclusão do desenho VDP1, composição
e apresentação, alinhando a apresentação ao relógio emulado. Não se deve
forçar o retorno do poll nem inferir que essa mudança, sozinha, corrigirá
o conteúdo visual.

Houve 19 imagens distintas, salvas em cerca de 4 MB de PPM. A última mudança
foi no quadro 18.269; até o quadro 23.184 o conteúdo permaneceu igual.
A inspeção visual confirma apenas faixas nos oito primeiros pixels de altura,
com o restante preto (2.560 pixels não pretos), sem tela reconhecível.

Artefatos: `build/artifacts/gfs-pcm-window-20m.txt`,
`build/artifacts/gfs-pcm-window-20m.wav` e
`build/artifacts/gfs-pcm-window-20m-frames/frame_018269.png`.
Build Release e CTest passaram; as mudanças desta etapa são diagnósticas.

### Separação entre desenho e apresentação

O teste confirmou a causa do descompasso de tempo. Antes, `VideoRuntime` chamava
o `frameSink` dentro de `advanceFrame()`. Como PTMR=1 chama `advanceFrame()` em
cada disparo manual, o frontend executava `paceFrame()` dezenas de milhares de
vezes, embora o scheduler tivesse produzido apenas 50 VBlanks.

Agora `advanceFrame()` somente executa/desenha e compõe `lastFrame_`. A nova
fronteira `presentFrame()` é chamada pelo scheduler imediatamente após o
`advanceFrame()` do VBlank. O sink do frontend, a captura PPM e o áudio passam
a observar somente frames apresentados, enquanto `framePending()`/`consumeFrame()`
continuam expondo o último frame composto para testes e consumidores internos.
O desenho PTMR=1 e o sinal de draw-end não foram removidos nem atrasados.

O teste de janela repetido com 20 milhões de blocos produziu:

- `video frames=23184` compostos internamente;
- `presented frames=50`, exatamente `vblank=50`;
- `SCSP audio frames=50`, `36750` amostras por canal e oito atualizações PCM;
- 26 publicações GFS, descritor final 26 e cursor de consumo 8.

O conteúdo visual permaneceu nas mesmas 19 imagens distintas, com 2.560 pixels
não pretos e faixas no topo. Portanto o problema de ritmo foi isolado e
corrigido, mas a composição VDP2/VDP1 ainda precisa de investigação separada.
O teste unitário `presentsOnlyAtSchedulerFrameBoundary` garante que um desenho
manual não chama o sink e que a apresentação ocorre na fronteira de VBlank.

Artefatos alinhados: `build/artifacts/gfs-pcm-window-20m-aligned.txt`,
`build/artifacts/gfs-pcm-window-20m-aligned.wav` e
`build/artifacts/gfs-pcm-window-20m-aligned-frames/frame_018883.png`.

### Snapshot de composição VDP1/VDP2

`VideoRuntime::presentFrame()` agora possui um sink diagnóstico opcional. Quando
`--frame-dir` é usado, cada VBlank produz
`<frame-dir>/video-diagnostics/frame_NNNNNN_{vdp2,vdp1_bank0,vdp1_bank1,final}.ppm`
e um `.txt` com os registros VDP1/VDP2, bounds dos pixels não transparentes e
faixas não nulas de VDP1 framebuffer, VDP2 VRAM e VDP2 CRAM. O banco 1 é
emitido junto com o banco 0, incluindo `display_bank`, `draw_bank` e
`swap_pending`; isso torna a troca manual por `FBCR` observável.

O teste curto de 1,6 milhão de blocos gerou quatro snapshots de VBlank em
`build/artifacts/video-diag-test/video-diagnostics`. No último snapshot, VDP2
estava habilitado (`TVMD=0x8010`, `BGON=0x0001`, `CHCTLA=0x0042`), mas a CRAM
estava vazia e os dois bancos VDP1 tinham zero pixels não pretos. A troca
manual de `FBCR` passou a ser exercitada separadamente por teste unitário e
continua explícita no snapshot.

O recorte representativo de 5 milhões de blocos (`build/artifacts/video-diag-5m-v2`)
é mais conclusivo: no quadro 2702, `vdp1.bank0.non_zero=0` e
`vdp1.bank1.non_zero=0`, enquanto `vdp2.composed.non_zero=2560` com bounds
`0,0-319,7`; `final.composed` tem exatamente os mesmos bounds. Logo as faixas
atuais vêm do caminho VDP2/BACK, não do overlay VDP1. A CRAM segue vazia, o
que descarta uma origem baseada em paleta neste recorte. Esse quadro segue
instrumentado, mas não é a causa do conteúdo ausente neste estado específico.

O decoder também deixou de tratar o bit 31 do fetch de 32 bits como alfa:
bitmap RGB de 24 bits agora interpreta os bytes VRAM como `B:G:R`, com byte
superior não pertencente à cor. O teste unitário
`composesVdp2Rgb888BitmapWithoutAlphaBit` cobre essa regra. A repetição de 5M
após a correção ainda produz `320×8`; a hipótese de alfa isolada foi
descartada.

O diagnóstico por linha fechou essa hipótese: no quadro 2702, `NBG0.base=0x0`,
`width=512`, `height=256`, `bits_per_pixel=32` e `row_pitch=2048`. As linhas
0–7 têm 320 pixels não nulos; as linhas 8–139 também possuem `first_raw`
válido na VRAM, mas zero pixels publicados. Ao mesmo tempo, `WCTLA=0x0003`,
`WPSX0=0`, `WPSY0=8`, `WPEX0=639` e `WPEY0=231`. No VDP2, o bit 1 de WCTLA
habilita W0 e o bit 0 seleciona a área interna de W0; a interpretação anterior
estava invertida e deixava visível exatamente `y=0..7`, reproduzindo o `320×8`
observado
([referência de WCTLA do manual VDP2](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p16_17.htm)).

Assim, base, pitch e conteúdo por linha estão coerentes; não devemos desligar a
janela globalmente. O trace de escritores e o modelo FBCR abaixo preservam a
semântica do hardware enquanto isolam a camada que deveria preencher a área
fora da janela.
O arquivo `.txt` agora registra também `vdp2.NBG0.window.enabled`, `outside` e
o retângulo efetivo para cada VBlank.

### Escritores de WCTLA e troca FBCR do VDP1

O trace de MMIO localizou a escrita de `WCTLA=0x0003`: ela ocorre no bloco
`0x06011F84`, no mesmo laço que configura `WPSX0/WPSY0/WPEX0/WPEY0`. No recorte
de 2 milhões de blocos, as escritas aparecem em `step=600044` e se repetem no
laço de vídeo (`build/artifacts/video-vdp1-fbcr-2m/vdp2-window-writes.txt`).
Isto é inicialização deliberada da janela, não corrupção do bitmap.

O mesmo recorte mostra a sequência VDP1 real em
`build/artifacts/video-vdp1-fbcr-2m/vdp1-register-writes.txt`:

- `0x06010342` escreve `TVMR=0` e `FBCR=3`;
- `0x06010DA6` escreve `TVMR=8` e `FBCR=3`;
- `0x060119EE` (chamador `0x060119E6`) dispara `PTMR=1` repetidamente.

O HLE agora mantém dois framebuffers, direciona escritas/command lists para o
banco de desenho e troca o banco visível somente em `presentFrame()`, quando há
um desenho pendente. O teste `swapsVdp1ManualFramebufferAtPresentation` cobre a
sequência verde (banco 1) → vermelha (banco 0), e o manual descreve `FBCR` como
o registrador de seleção das áreas de desenho e exibição
([referência de FBCR do manual VDP1](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p04_12.htm)).

O dump da lista em `build/artifacts/video-vdp1-fbcr-2m/vdp1-command-list.txt`
tem 25 comandos, mas somente cinco polígonos: o único polígono interno é
preto/transparente e os quatro restantes são bordas fora da área útil. Os dois
bancos permanecem sem pixels não pretos e o quadro final segue vazio; portanto
a troca FBCR estava faltando, mas não é a camada que preenche a região externa
neste estado da ISO. A janela não foi desligada e base/pitch não foram alterados.
O próximo alvo, agora isolado, é localizar a publicação da camada/prioridade
que deveria fornecer esse preenchimento depois da inicialização de WCTLA.

### Publicação VDP2: BACK, janela e prioridade

O compositor agora segue a publicação usada pelo caminho de software do
Yabause: o BACK é preenchido primeiro, NBG0/NBG1 são mesclados por pixel com
`PRINA` (prioridade zero explícita desabilita a camada), e empates entre NBG0 e
NBG1 seguem a ordem de camada. A regra de BACK também respeita `TVMD.DISP`,
`TVMD.BDCLMD`, `BKTAU.BKCLMD` e `VRSIZE`; o snapshot registra o endereço efetivo,
modo e amostras da tabela. A referência do Yabause é
[`Vdp2DrawBackScreen`](https://raw.githubusercontent.com/Yabause/yabause/master/yabause/src/vidsoft.c#L1440-L1487).

O erro visual era a janela, não o FBCR: corrigir `W0A=1` como área interna faz
`WCTLA=0x0003` publicar NBG0 dentro de `0,8–639,231`. O teste de 5 milhões de
blocos em `build/artifacts/video-vdp2-window-5m` passou de 2.560 para 42.505
pixels VDP2 não pretos, com bounds `0,8-319,192`; os dois bancos VDP1 continuam
vazios. O BACK do jogo é modo cor única, endereço `0x6FFFE`, palavra `0x0000`,
portanto ele permanece preto e não é a fonte das faixas. O trace focado de
registradores/tabela fica em `vdp2-layer-writes.txt`.

### Limite real do bitmap NBG0

> Nota histórica: a hipótese de truncamento abaixo foi corrigida pelo mapa de
> writes por chamada descrito em “Correção da leitura do limite `0x46000`”.

O snapshot passou a separar `raw_non_zero`, `alpha_non_zero` e pixels
publicados por linha. No frame 667, as linhas 140–191 têm
`raw_non_zero=0` e `alpha_non_zero=0`; não há dado RGB nem bit de transparência
para o decoder descartar. A linha 139 termina em `0x45FFE` e a linha 140
começaria em `0x46000`.

O trace de fronteira (`build/artifacts/video-vdp2-boundary-2m/vdp2-layer-writes.txt`)
mostra a inicialização até `0x45FFE`, seguida por escritas de serviço no mesmo
limite, mas nenhuma escrita de bitmap em `0x46000`–`0x47FFF`. Portanto o
próximo bloqueio é o produtor/transferência que entrega somente 140 linhas ao
NBG0; alterar RGB888, alpha, janela ou prioridade agora apenas mascara a falta
de conteúdo na VRAM.

### Limite confirmado no descriptor SCU DMA

> Nota histórica: `count=0x46000` cobre exatamente 224 linhas de stride
> `0x500`; ele não representa uma imagem de apenas 140 linhas.

O trace agora reconstrói os descriptors SCU que publicam no destino VDP2
`0x25E00000`, em `scu-dma-writes.txt`, além de manter o log bruto dos writes.
Com o período de VBlank padrão (`--vblank-blocks 10000`), o primeiro descriptor
observado usa `source=0x06009F6C` e `count=0x80000`. Os descriptors que se
repetem durante a publicação do frame usam `source=0x060B49CC`,
`add=0x101`, `mode=0x7` e, crucialmente, `count=0x46000`:

```
summary.nbg0_dma channel=0 ... source=0x060B49CC destination=0x25E00000 count=0x46000 add=0x101 mode=0x7
```

Como o contador é em bytes, esse DMA escreve os offsets de destino
`0x00000`–`0x45FFF`; ele termina exatamente antes de `0x46000`. Não há um
descriptor posterior para `0x46000`–`0x47FFF` nesse caminho. Assim, a ausência
das linhas 140 em diante é causada pelo limite do buffer publicado pelo produtor, antes do
decoder RGB888; mudar o formato de cor não pode criar os bytes que nunca são
transferidos.

### Origem do limite no buffer

Para distinguir truncamento do descriptor de falta de dados na origem, o trace
também captura o descriptor na entrada de `0x06034A84` e amostras relativas ao
source. No descriptor alternativo usado pelos frames:

```
source=0x060B49CC destination=0x25E00000 count=0x46000
+0x45FFC: 0x80000000  +0x46000: 0x00000000  +0x46004: 0x00000000
+0x6FFFC: 0x00000000  +0x70000: 0x00000000
```

O último word da origem ainda contém o marcador `0x80000000`, mas o byte
seguinte já está zerado. O descriptor, portanto, corresponde ao conteúdo que
o buffer realmente recebeu; o SCU não está interrompendo uma transferência
maior. O próximo alvo passa a ser o carregamento GFS/CD que preenche
`0x060B49CC` (e a dimensão/quantidade publicada por esse produtor), não o
runtime DMA nem o decoder RGB888. O snapshot completo fica em
`build/artifacts/video-vdp2-descriptor-probe-1m3-trace2/scu-dma-descriptors.txt`.

### Limite confirmado no preenchimento GFS/CD

> Nota histórica: o fim em `base+0x46000` é o fim esperado de
> `224 * 0x500`; a captura posterior comprovou que todas as linhas são
> escritas.

A instrumentação do decompressor que escreve `0x060B49CC` separa os writes por
blocos de 0x1000 e captura os contadores na entrada e na saída do laço. No
recorte de 1,3 milhão de passos (`build/artifacts/video-vdp2-gfs-setup-probe-1m3-v3`),
há `136608` writes (`135448` não-zero) nos chunks 0–69, e nenhum write nos
chunks a partir do offset `0x46000`. O último write ocorre no passo `822791`,
em `+0x45FFC`, ainda no bloco `0x06041D90`.

Na entrada, o produtor inicializa `R4=0x060B49CC`, `R6=0x50` e `R7=0x1C`,
com a origem comprimida em `R14=0x0608BD08`. Após o último lote, o bloco
`0x06041E9C` registra `R4=0x060FA9CC` (`base+0x46000`), `R7=0` e os contadores
da pilha zerados; `0x06041EA6` retorna imediatamente no passo seguinte. Isso
prova que `0x46000` é o limite produzido pelo próprio GFS/CD, antes da criação
do descriptor em `0x06034E54`/`0x06015D60`. Não há base, pitch, RGB888 ou runtime
DMA para corrigir nesse ponto: o próximo alvo é descobrir onde a chamada ao
decompressor recebe esses parâmetros e por que a dimensão publicada é essa.

### Configuração da tarefa que chama o decompressor

O trace seguinte capturou a escrita do objeto em `0x0606E45C`, a chamada
indireta em `0x060421C4` e a entrada de `0x06041D14`. O bloco configurador
`0x06042698` (chamado por `0x06042610`) grava:

```text
object+0x5C = 0x06041D14
object+0x60 = 0x060B49CC   # R4, destino
object+0x64 = 0x00000500   # R5, stride/linha antes da normalização
object+0x68 = 0x00000140   # R6, contador antes da normalização
object+0x6C = 0x00000070   # R7, dimensão antes da normalização
object+0x70 = 0x0608BD08   # R14, origem comprimida
```

Na entrada de `0x06041D14`, o chamador `0x060421C4` repassa exatamente
`R4=0x060B49CC`, `R5=0x500`, `R6=0x140` e `R7=0x70`. A própria rotina então
faz `R6 >>= 2` (`0x50`), `R7 >>= 2` (`0x1C`) e
`R5 = (0x500 - 0x140) << 2` (`0xF00`), antes de entrar em `0x06041D48`.
Assim, `0x50`, `0x1C` e `0xF00` não são valores inventados pelo runtime: são a
normalização determinística dos quatro campos escritos pelo produtor.

O mesmo caminho reaparece no passo `1172769` pelo bloco `0x06042702` (chamador
`0x06042614`), com callback `0x06041EB6` e outra origem comprimida
(`0x06091190`), mas com os mesmos `0x500/0x140/0x70`. O trace completo das
escritas do objeto e dos snapshots de registradores está em
`build/artifacts/video-vdp2-gfs-task-probe-1m3-v2/gfs-task-config-writes.txt`
e `gfs-decode-calls.txt`. O próximo ponto de investigação é a preparação dos
campos `R10/R11/R7` imediatamente antes de `0x06042698/0x06042702`; essa
preparação é detalhada na seção seguinte. Não foi necessário alterar DMA,
decoder ou o tamanho publicado.

### Origem dos parâmetros no parser GFS

O snapshot de `0x0604241A` (a entrada efetiva observada da rotina iniciada em
`0x06042418`) mostra os argumentos que chegam do chamador `0x0604045E`:

```text
R4 = 0x0608B6FC   # registro/fluxo GFS
R5 = 0x060B49CC   # base de destino
R6 = 0x00000500   # stride
R7 = 0x00000018   # estado/índice do parser
R8 = 0x0607C000   # objeto da tarefa
```

Em `0x0604242A`, `R10` recebe `R6`, portanto o `0x500` copiado por
`0x06042698/0x06042702` vem diretamente desse argumento. `R13` recebe `R4` e
avança pelos registros GFS; no primeiro registro relevante ele aponta para
`0x0608B708`, cujo conteúdo começa por
`0x10002A1C, 0x00000000, 0x00700140`. Em `0x06042530–0x06042542`, o parser
lê os campos de 16 bits: `R7` passa a `0x70` (palavra em `R13+8`) e a palavra
em `R13+10` produz `0x140` no `stack[8]`.

`R11` é zerado no prólogo (`0x0604243C`) e, no caminho do tipo `0x10`, recebe
em `0x06042558` a base salva de `R5`; por isso chega a `0x060B49CC` em
`0x060425C2`. Para registros posteriores, o parser soma os deslocamentos
calculados (`MUL.L`/`ADD`) a essa mesma base. Os configuradores apenas copiam
esses valores: `+0x60=R11`, `+0x64=R10`, `+0x6C=R7`, e `+0x70=R13+4` (no
primeiro trabalho, `0x0608BD08`).

Assim, os ponteiros e dimensões publicados são formados no parser a partir do
registro GFS e dos argumentos do chamador; não há evidência de truncamento ou
alteração no runtime DMA. A captura completa está em
`build/artifacts/video-vdp2-gfs-parser-probe-850k/gfs-decode-calls.txt`.

### Chamador `0x0604045E` e slot local da origem

O probe seguinte confirmou a preparação imediatamente anterior ao parser. No
passo `786935`, em `0x0604045E`, os valores lidos foram:

```text
R9  = 0x0607C004
R13 = 0x060801CC
SP  = 0x06009F10
R9+0x0C = 0x060B49CC   # R5
R9+0x10 = 0x00000500   # R6
R13+0x24 = 0x00000018  # R7 inicial
SP+0x04 = 0x0608B6FC    # slot consumido por R4 em 0x0604047C
```

O slot não é preenchido pelo DMA: `0x0604029A` grava repetidamente
`0x0608B6FC` em `0x06009F14` (`SP+4`), com chamador `0x06040246`. Depois de
cinco pushes, `0x0604047C` lê esse mesmo local como `@(24,R15)` e o passa em
`R4`. A dimensão inicial `0x18` vem do byte em `R13+0x24`; o parser posterior
substitui-a pelo `0x70` do registro GFS (`R13+8`).

Os registros estão em
`build/artifacts/video-vdp2-gfs-caller-probe-850k/gfs-decode-calls.txt` e
`gfs-parser-stack-writes.txt`.

### Cálculo de `R2` e tabela em `0x060801CC`

O probe seguinte capturou os blocos `0x0604022C`, `0x06040246` e
`0x0604029A`, além dos campos lidos do objeto em `R14=0x0607C038`. No primeiro
trabalho relevante, os valores usados pelo cálculo são:

```text
object+0x0C = 0x00000001   # índice da entrada
object+0x14 = 0x000008F0   # base somada ao deslocamento da tabela
object+0x64 = 0x060811CC   # base usada como R1
object+0x6C = 0x00001000   # valor subtraído como R3
object+0x70 = 0x0002E000   # limite usado como R6
R13         = 0x060801CC   # início da tabela
```

Em `0x0604022C`, o índice `1` é deslocado duas vezes por `SHLL2` (vezes 16)
e somado a `R13+0x40`, produzindo o ponteiro de entrada `R5=0x0608021C`.
Os quatro words dessa entrada são `0x0000AC40, 0x0000546C, 0x00000000,
0x00000002`; o primeiro é o deslocamento usado para formar `R4`:

```text
R4 = object+0x14 + table[0] = 0x8F0 + 0xAC40 = 0xB530
R2 = (R4 - object+0x6C) + object+0x64
   = (0xB530 - 0x1000) + 0x060811CC
   = 0x0608B6FC
```

Na entrada de `0x0604029A`, o snapshot confirma `R2=0x0608B6FC`; a instrução
`MOV.L R2,@(4,R15)` publica exatamente esse valor no slot `SP+4`, que depois
chega a `R4` em `0x0604047C`. Portanto, o registro GFS não é produzido pelo
DMA nem pelo decoder: ele é calculado pelo parser a partir da tabela em
`0x060801CC` e dos cinco campos do objeto. Os snapshots completos ficam em
`build/artifacts/video-vdp2-gfs-r2-probe-850k-b/gfs-decode-calls.txt`.

### Produtores do objeto e da entrada da tabela

O trace de writes acompanha diretamente os cinco campos consumidos pelo
parser e os 16 bytes da entrada `0x0608021C`. A sequência observada é:

```text
step 504133  0x0603EC26 (caller 0x0603EB88)
  object+0x70 = 0x00029800
  object+0x64 = 0x060801CC
  object+0x0C = 0

step 512800..512803  0x0603BC14 (laço)
  table+0x00..0x0C = 0xAC40, 0x546C, 0x0000, 0x0002

step 518530    0x0603F500 (caller 0x0603F4EC)
  object+0x14 = 0x000008F0
step 522723    0x0603F526 (caller 0x0603F518)
  object+0x6C = 0x00001000
step 523286    0x0603F656 (caller 0x0603F644)
  object+0x70 = 0x0002E000
  object+0x64 = 0x060811CC
step 718645    0x0604073E (caller 0x0604073A)
  object+0x0C = 1
step 822822    0x06040766 (caller 0x06040762)
  object+0x0C = 2
```

O laço `0x0603BC14` executa `MOV.L @R13,R2` seguido de `MOV.L R2,@R14`,
com `R13=0x25818000` e `R14=0x0608021C`; portanto, a entrada não é montada
pelo decoder, mas copiada de uma origem já preenchida. Os campos do objeto são
montados posteriormente pelo caminho de preparação de tarefa, e o índice é
incrementado pelos caminhos de produtor inicial/subsequente. O trace completo,
incluindo bloco, chamador e registradores, está em
`build/artifacts/video-vdp2-gfs-object-writes-850k/gfs-parser-data-writes.txt`.

### Origem `0x25818000`

Como `0x25818000` pertence ao FIFO do CD Block (alias de `0x05818000`), não
há uma escrita SH-2 convencional nessa faixa. O laço `0x0603BC14` faz quatro
leituras do FIFO enquanto `R14` percorre `0x0608021C..0x0608022C`. O estado do
CD identifica a transferência como `LBA=129671` (`FAD=129821`), comando
`0x61`, partição 0; o FIFO começa em `0x50` e termina em `0x60`:

```text
step 512800: 0x0000AC40
step 512801: 0x0000546C
step 512802: 0x00000000
step 512803: 0x00000002
```

Esses valores vêm da transferência de setor preparada pelo CD runtime a partir
da ISO; no MDF de 2448 bytes isso corresponde ao offset bruto `0x60` do setor
(o runtime remove os 16 bytes de cabeçalho antes de expor o FIFO). O setor
começa com `FILEM` e contém `STAB` em `0x40`, confirmando que os quatro words
fazem parte do registro de tabela do próprio recurso. O emulador não fabrica a
entrada na rotina de cópia. O trace das leituras, com LBA/FAD, deslocamento do
FIFO e registradores `R13`/`R14`, está em
`build/artifacts/video-vdp2-gfs-source-probe-850k-d/gfs-parser-source-reads.txt`.

### Relação STAB → descritor GFS → limite `0x46000`

A leitura bruta do setor `LBA=129671` permite seguir a entrada sem inferência
do runtime. O cabeçalho `STAB` está no offset bruto `0x40`; a tabela começa em
`0x50`. Como o FIFO expõe o setor sem os 16 bytes iniciais, o registro copiado
para `0x0608021C` (FIFO `+0x50`, bruto `+0x60`) é:

```text
STAB[1] = offset 0x0000AC40, length 0x0000546C,
          flags 0x00000000, type 0x00000002
```

Os quatro words são idênticos aos de `0x0608021C`. Quando o objeto em
`0x0607C038` está com `index=1`, os blocos `0x0604074C` e `0x060401D4`
selecionam essa entrada e publicam a primeira unidade (`pubs=1`, passos
`786910`/`786912`). O campo `offset=0xAC40` também participa do cálculo que
produz `R2=0x0608B6FC`, a origem do registro GFS consumido em `0x0604045E`.

Depois do incremento do índice, `index=2` seleciona `0x0608022C`, que é o
registro STAB seguinte:

```text
STAB[2] = offset 0x000100AC, length 0x00002B48,
          flags 0x80000002, type 0x00000002
```

Isso aparece no primeiro caminho não-publicador (passos `827400`/`827402`,
`r0=0x80000002`). Portanto a tabela STAB está sendo consumida corretamente e
determina quais recursos GFS são visitados; não há sinal de que a cópia do CD
esteja truncando a entrada.

É importante separar três medidas que antes pareciam uma só:

```text
STAB[1]             offset 0xAC40, length 0x546C  (recurso no arquivo)
GFS decoded target   0x060B49CC .. 0x060FA9CB     (saída do decompressor)
SCU DMA publicado    count=0x46000                (0 .. 0x45FFF)
```

O último write do decompressor ocorre em `0x060FA9C8` (`base+0x45FFC`) no
bloco `0x06041D90`; `base+0x46000` já está zerado quando o descriptor SCU é
montado. Logo, `0x46000` é o tamanho da saída decodificada/publicada para esse
trabalho, e não o `length` do registro STAB (`0x546C`) nem um corte do FIFO.
O caminho causal agora está reduzido a `STAB[1]` → parser GFS → parâmetros do
decompressor (`0x500/0x140/0x70`) → limite de saída em `0x06041D90`. O próximo
instrumento deve capturar como esse callback transforma o `length`/tipo STAB em
esses parâmetros e por que a saída termina em `0x46000`.

Os eventos de seleção/publicação estão em
`build/artifacts/video-vdp2-gfs-source-probe-850k-d/run.txt`; os quatro words
copiados do FIFO estão em
`build/artifacts/video-vdp2-gfs-source-probe-850k-d/gfs-parser-source-reads.txt`.

### Instrumentação do callback STAB → decompressor

O probe passou a capturar a cadeia no próprio instante em que ela acontece,
sem depender do valor final do índice no objeto. Em `0x0604241A` o parser salva
o índice e os quatro words STAB antes de o produtor avançar `object+0x0C`; em
seguida o trace associa os configuradores `0x06042698/0x06042702`, a entrada
`0x06041D14` e a saída de `0x06041E9C`. O novo arquivo é
`build/artifacts/video-vdp2-gfs-stab-callback-1300k/gfs-stab-callback.txt`.

O resultado de `1,3M` passos mostra quatro entradas STAB e o mesmo formato de
saída:

```text
index=1  STAB offset=0xAC40  length=0x546C  flags=0       type=2
         parser R10=0x546C  -> task 0x500/0x140/0x70
         -> 0x06041D14 R4/R5/R6/R7 = 0x060B49CC/0x500/0x140/0x70

index=2  STAB offset=0x100AC length=0x2B48  flags=0x80000002 type=2
         parser R10=0x2B48  -> task 0x500/0x140/0x70

index=4  STAB offset=0x1821C length=0x2C24  flags=0x80000004 type=2
         parser R10=0x2C24  -> task 0x500/0x140/0x70

index=5  STAB offset=0x1AE40 length=0x546C  flags=6       type=2
         parser R10=0x546C  -> task 0x500/0x140/0x70
```

Isso é a evidência que faltava: `STAB.length` é carregado no callback (`R10`),
mas não vira diretamente o tamanho do bitmap. O tipo `2` chega no registro GFS
como `gfs_words[2]=0x00020000`; os campos de formato/dimensão do registro
(`gfs_words[1]/[5]`) permanecem `0x014000E0`/`0x00700140`, e o parser produz
`0x140` de largura e `0x70` de altura para todos esses recursos. O primeiro
trabalho então percorre `0x06041D90` até `base+0x46000`; os demais já estão
selecionados/configurados, mas ainda não entram no decompressor dentro do
recorte de `1,3M` passos.

O próximo ponto assertivo não é mais o CD: é o bloco `0x06042530`/`0x060425C2`,
que lê os campos 16-bit do registro GFS e os transforma em `R7=0x70`,
`stack+8=0x140` e, depois, no `+0x68` do objeto. O callback já está
instrumentado; a próxima alteração funcional só deve ser feita depois de
confirmar se o `0x46000` é uma dimensão fixa do tipo `2` ou um limite aplicado
quando o primeiro registro termina.

### Captura prolongada sem truncar o relatório

O limite do vetor de snapshots de configuração foi ampliado de 256 para 4096
entradas. Na execução de `2M` passos (`build/artifacts/video-vdp2-gfs-stab-callback-2m-v2`),
o trace alcançou `STAB[17]` e registrou novas entradas do decompressor para os
índices 6, 11, 12 e 14. Todas continuam entrando como
`R4/R5/R6/R7 = 0x060B49CC/0x500/0x140/0x70`; o buffer de saída é reutilizado,
não criado com um tamanho derivado de `STAB.length`.

Em paralelo, cada publicação SCU continua usando `source=0x060B49CC` e
`count=0x46000`, enquanto os writes GFS posteriores sobrescrevem esse mesmo
buffer (794.688 writes acumulados no recorte). Isso elimina o falso diagnóstico
de que o relatório parava no primeiro recurso: o jogo está avançando por vários
recursos STAB, e o produtor recicla o mesmo alvo NBG0.

### Correção da leitura do limite `0x46000`

O mapa de writes por chamada do decompressor corrigiu a interpretação anterior.
No primeiro retorno completo, a chamada escreveu `71680` words, com
`max_written_row=223` e `max_written_offset=0x45FFC`; as linhas 0, 139, 140 e
223 receberam 320 words. Como `0x46000 = 224 * 0x500`, o produtor, o
decompressor e o descriptor SCU entregam as 224 linhas contíguas. O descriptor
também confirma `source=0x060B49CC`, `destination=0x25E00000`,
`count=0x46000`, `add=0x101`, `mode=0x7`.

O corte visual vinha do endereço de leitura do VDP2, não de uma transferência
curta. `CHCTLA=0x0042` descreve a janela física 512x256/32 bpp; o jogo configura
`SCRCTL=0x0007`, `LSTA0=0x60000` e `VCSTA=0x70000` e preenche as tabelas de
line-scroll e vertical-cell-scroll. Esse é o método documentado pela Sega para
exibir um bitmap RGB de 320x224 com endereços VRAM contínuos:
[exemplo do manual VDP2](https://docs.exodusemulator.com/Archives/SSDDV25/segahtml/hard/vdp2/hon/p15_40.htm),
[boletim técnico ST-058](https://antime.kapsi.fi/sega/files/ST-058-R2-060194.pdf).

O compositor agora aplica, por linha, o par horizontal/vertical da tabela
`LSTA0` e soma o valor de cada célula de 8 pixels da tabela `VCSTA` antes de
calcular o endereço dentro da abertura 512x256. A captura fica registrada nos
diagnósticos (`vdp2.SCRCTL`, `vdp2.LSTA0*`, `vdp2.VCSTA*` e amostras das tabelas)
e há um teste dedicado para a transição física em `x=192`.

Esse também é o caminho adotado pelos renderizadores de software do Yabause:
line-scroll/vertical-cell-scroll são interpretados antes de compor o NBG0; o
próprio histórico do projeto registra a inclusão desse interpretador
([ChangeLog do Yabause](https://github.com/Yabause/yabause/blob/master/yabause/ChangeLog)).
