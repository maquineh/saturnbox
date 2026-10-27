# Leituras do mapa NBG0

Continuação: [origem do mapa do título e correções do runtime](../ROCKMAN-X4-TITLE-MAP-FIX.md).
As hipóteses abaixo são o registro histórico da investigação; a continuação
identifica o bloqueio de SEEK e distingue o primeiro setup em células do título.

Esta captura acompanha o conteúdo do mapa esperado em
`0x70000–0x71FFF` e as palavras de nome que o renderizador realmente lê
durante a composição de NBG0. O objetivo é separar o momento em que o mapa é
apagado da ativação do modo célula.

## Captura

```powershell
& build/tools/saturn-sh2/Release/saturn-sh2.exe boot build/rockman/aip.bin `
  --aip-base 0x06002E40 --ip build/rockman/ip.bin --ip-base 0x06002000 `
  --program build/rockman/0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf' --disc-sector-size 2448 `
  --continue-after-execdmp --vblank-blocks 10000 --trace-game --steps 12000000 `
  --vdp2-map-read-trace build/artifacts/map-read-trace-20260920-g
```

Os arquivos são `vdp2-map-state.csv` e `vdp2-map-read.csv` em
`build/artifacts/map-read-trace-20260920-g/`. O primeiro tem uma linha por
amostra de composição e registra os registradores e o número de palavras não nulas no
mapa esperado. O segundo agrega as leituras de palavras de nome feitas pela
composição em modo célula.

## Sequência observada

| amostra | estado do VDP2 | words não-zero em `0x70000–0x71FFF` |
| --- | --- | ---: |
| frame 53, passo 516017 | `TVMD=0x0010`, `BGON=0` | 43 palavras não nulas |
| frame 57, passo 520442 | `TVMD=0x8010`, `BGON=1`, `CHCTLA=0x0042` (bitmap) | 43 |
| frame 2192, passo 8673481, antes da limpeza | `TVMD=0x8010`, `BGON=1`, `CHCTLA=0x0042` (bitmap) | 43 |
| frame 2193, após o passo 8673634 | `TVMD=0x8010`, `BGON=1`, `CHCTLA=0x0042` (bitmap) | 0 |
| frame 2451, passo 11249999 | `TVMD=0x0010`, `BGON=1`, `CHCTLA=0x0101` (célula) | 0 |
| frame 2458, passo 11260842 | `TVMD=0x8010`, `BGON=0x000F`, `CHCTLA=0x0101` (célula) | 0 |

Entre a carga do bloco `0x0601667C` e a limpeza de `0x06034B5C`, o NBG0 fica
em modo bitmap. Portanto, o renderizador não lê o mapa de nomes durante o
período em que as 43 palavras estão presentes. A limpeza ocorre antes da
troca para `CHCTLA=0x0101`.

Na captura inicial, antes da correção da base, há 1030 amostras entre os
frames 2458 e 3413. Cada uma faz 143360 leituras de palavras de nomes,
todas zero, no intervalo físico `0x40000–0x406CE`. Esse era o endereço
calculado pelo renderizador; a base conforme o manual é `0x70000`.
Não houve palavra não nula restante no mapa esperado `0x70000–0x71FFF`.

## Conclusão

O mapa fica zerado antes de a execução capturada entrar no modo célula. Na
captura original há dois fatos observáveis:

1. a CPU limpa 43 palavras em `0x70000–0x71FFF` no passo `8673634`;
2. quando NBG0 passa a ser célula, o renderizador antigo lê `0x40000` em vez
   de `0x70000`.

O trace não altera a fórmula nem o fluxo do jogo. A limpeza ocorre enquanto
NBG0 ainda está em bitmap; isoladamente, ela não mostra que esses valores
fossem o mapa do título.

Validação: build Release e CTest passaram (`1/1`).

## Correção da base em 26/09/2026

O compositor agora calcula o tamanho da página de nomes conforme o tamanho
do caractere: caracteres 16x16 usam um quarto das entradas de caracteres
8x8. O alinhamento continua seguindo o tamanho do plano, e o endereço é
limitado à VRAM de 4 Mbit implementada pelo runtime. A regra segue a
[tabela 4.8 do manual VDP2](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_80.htm).
Para os registradores desta captura, a base passa a ser `0x70000`.

Um teste de renderização cobre 24 combinações: NBG0/NBG1, caracteres 8x8/16x16,
nomes de uma/duas palavras e planos de 1x1, 2x1 e 2x2 páginas. As bases
esperadas são constantes independentes da fórmula do compositor; um nome
na base esperada seleciona uma paleta vermelha, enquanto a base errada não.
Build Release e CTest passaram (`1/1`).

Os CSVs acima pertencem à captura inicial, anterior à correção do endereço.
A reprodução completa após a correção está documentada abaixo; ela não altera
a navegação entre páginas.
A limpeza observada, por si só, não prova defeito no fluxo do jogo: durante
o bitmap, `VCSTA=0x70000` também aponta para essa região como tabela de
vertical-cell-scroll (ver ROCKMAN-X4.md). A captura atualizada e o teste com
Start estão documentados abaixo.

### Boot completo com endereço corrigido — 26/09/2026

Repeti o boot até o limite de 12 milhões de passos com o renderizador corrigido,
uma vez sem botão e outra com Start mantido. Os resultados do mapa e a sequência
de comandos de CD foram iguais; os CSVs de estado e leitura são byte a byte
idênticos:

- o gatilho capturou o frame preto 2469, no passo `11269999`;
- NBG0 leu 143360 words de nomes entre `0x70000` e `0x706CE`; nenhuma era
  não-zero;
- os 4096 words da região `0x70000–0x71FFF` também eram zero;
- `CHCTLA=0x0101`, `BGON=0x000F`, e NBG0, VDP2 e a saída final tinham zero
  pixels não pretos;
- os 139 descritores do FMV foram consumidos, e o boot terminou por limite de
  passos, no bloco `0x060303CC`.

O histórico de CD terminou com `PLAY` em FAD 378 por 276 setores, a leitura de
`X4LOW.X4` já identificada no checkpoint do FMV. Não houve um `PLAY` para
FAD 130776 (`OP.CPK`) nessas execuções. Manter Start desde o boot não alterou
o estado observado. Portanto, a captura confirma que o renderizador agora lê
o endereço correto e que a região não recebeu nomes antes do frame capturado;
ela não localiza uma rotina que tenha carregado o mapa de nomes do título.

O `PLAY` anterior do histórico, FAD 654 por 46 setores, corresponde a
`ASCII.ARC` na ISO (LBA 504, 94208 bytes). Extraí a imagem para
`build/artifacts/ASCII.ARC`. A sequência depois dela inclui `X4LOW.X4`, mas
não mostra leitura de `CAPCOM.ARC`, que começa no LBA 550 (FAD 700); esse
arquivo foi extraído para `build/artifacts/CAPCOM.ARC` para inspeção posterior.
Isso localiza os arquivos lidos e não lidos nesse recorte; ainda não demonstra
qual deles contém o mapa do título nem qual comando deveria carregá-lo.

Os artefatos das duas execuções são:

- sem botão: `build/artifacts/first-black-chain-20260926/`,
  `build/artifacts/map-read-trace-20260926/` e
  `build/artifacts/map-read-trace-20260926-run.txt`;
- Start mantido: `build/artifacts/first-black-chain-start-20260926/`,
  `build/artifacts/map-read-trace-start-20260926/` e
  `build/artifacts/map-read-trace-start-20260926-run.txt`.

O último bloco seleciona o índice 64 no dispatcher `0x0603084C`. A rotina
dinâmica identificada em `0x0022750C` copia 96 words da imagem `X4LOW.X4`
para `0x25C7FF00`, no espaço VRAM do VDP1; esse caminho não escreve o mapa
NBG0. O próximo ponto de rastreamento é descobrir quem registra e aciona a
carga posterior de recursos de tela e se a espera no dispatcher depende de
uma transição de estado ainda não reproduzida.

### Retorno do resolvedor e espera pelo primeiro setor — 26/09/2026

A captura parcial foi limitada a 11.900.000 passos, abaixo do limite de 12
milhões. O callback de título continua no índice 5 (`0x06016DA6`). No passo
11255502, ele enfileira o ponteiro `0x06043EB0`, que aponta para `SNDCOM.ARC`,
e grava `count=1`; no passo 11255503, ativa `busy=1` em `0x0605E7F6`.

O worker começa em `0x0601CE72` no passo 11255548 e chama o GFS. O resolvedor
indireto `0x0603AD4A` retorna `R0=0x14` (índice 20 da tabela de nomes) no passo
11255816. O chamador segue pelo ramo de sucesso; `0x0601C5F0` recebe novamente
`R0=0x14` no passo 11255834. O caminho entra em `0x0601C604`, passa por
`0x0601C65C`/`0x0601C650` e chega a `0x0601C678` no passo 11256735. Esse bloco
chama `0x0601CC7E`; a execução permanece no laço de consulta interno e não
retorna ao trecho seguinte do worker (`0x0601C6C0`).

O laço em `0x0601CD0E` compara o valor lido de `0x06053354` com o limite
`0x800`. A captura adicional registrou 1.530 escritas nesses dois campos,
repetidas em pares durante a espera. A rotina de status `0x06035FCC` copia
`1` do estado do objeto `0x0606A854` para `0x06053350` em `0x06035FE4`; o
caminho `0x0603601A` grava `0` como quantidade concluída em `0x06053354`.
No passo 11899186, o contador ainda é zero; logo depois, `0x0601CC9E` observa
o sinal ativo e volta a esperar. A consulta de status está funcionando, mas
nenhum byte chega ao objeto que ela consulta.

O resolvedor segue pelo caminho de sucesso do dispatcher GFS: a captura tem
seis passagens em `0x06035790` e nenhuma no erro `0x060357A2`. A sequência
estática após o retorno chega a `0x0601C6AA`, que chama `0x06035DC2` com o
handle `0x0606A854`, o buffer `0x06052350` e o limite `0x800`; em seguida,
o worker espera em `0x0601CC7E`. Portanto, a chamada interna de leitura foi
feita, mas nenhum comando CDC `0x61` ou `0x63` para o recurso aparece depois
dela. O método de atualização `0x0603A646`, visto abastecendo o fluxo do FMV,
teve sua última chamada no passo 8670417 e não volta a aparecer nesta execução.

O próximo recorte deve observar o ramo de `0x06035DC2` em `0x06035E10` e a
rotina `0x06036BD0`. A disassembly testa o word em `handle+0x28`; se for zero,
reinicializa os campos do anel. Se não for zero, chama o helper fixo
`0x0603C006` (literal em `0x06036C14`); o valor em `handle+0x28` não é usado
como destino direto do `JSR`. Ainda falta determinar qual ramo ocorre para o
handle do título e como ele deveria chegar ao produtor GFS/CDC.

Até o limite de 11,9 milhões, não houve GET DATA (`0x61`) nem GET+DELETE
(`0x63`) após o enfileiramento. O índice da fila (`0x0605E7F4`) não avança, o
callback de conclusão `0x060101B6` não é chamado e `busy` não é limpo; o estado
final permanece `index=0`, `count=1`, `busy=1`, com `SNDCOM.ARC` no primeiro
slot. O primeiro PLAY posterior ao resolvedor ocorre no passo 11255941 e pede
FAD 378 (`X4LOW.X4`), não FAD 12606 de `SNDCOM.ARC`. O carregador
`CAPCOM.ARC` (`0x0601D568`) não é executado. Portanto, o primeiro pedido de
setores do trabalho enfileirado e a carga do mapa do título ainda não foram
alcançados; o worker continua preso no polling de `0x0601CC7E`, aguardando bytes
que não foram publicados. A captura do ramo está descrita abaixo; falta agora
localizar a publicação dos bytes para esse handle antes do polling.

### Ramo do handle GFS do título — 27/09/2026

Nova captura parcial, limitada a 11.900.000 passos, registrou o estado do
handle `0x0606A854` e os desvios nas rotinas de setup. No passo 11256890,
`0x06035F06` chama `0x06036BD0` com `R4=0x0606A860` (handle+12). O campo
`handle+0x28` vale zero. A rotina segue o ramo `0x06036BE8` e inicializa os
campos do anel: `+0x5C=0`, `+0x60=0xA3`, `+0x64=0`. Ao retornar, `0x06035F0A`
grava estado 2 em `handle+4`.

No passo 11257222, a leitura de `0x06035DC2` recebe o mesmo handle, buffer
`0x06052350` e limite `0x800`; o estado ainda é 2 e `handle+0x28` continua zero.
Em `0x06035E10`, o `TST` desse estado seguido de `BF/S` desvia para `0x06035E20`,
pulando a chamada a `0x06036BD0`; em seguida, o código grava estado 1 em
`handle+4` e começa o polling. Portanto, nesta leitura a rotina de setup do anel
não inicia um produtor e não há chamada ao helper `0x0603C006`.

Até o teto da execução, o pedido de setores associado à fila do título continua
em zero, embora haja 723 comandos `0x61` de outras partes do fluxo. A fila segue
`index=0`, `count=1`, `busy=1`, com `SNDCOM.ARC` pendente; `0x06053354` permanece
zero. A rotina do mapa continua sem ser alcançada. O que falta agora é localizar
quem deveria preparar o word `handle+0x28` ou publicar os bytes no buffer do
handle antes de `0x0601CC7E` esperar.

Captura: `build/artifacts/title-gfs-handle-setup-119m-20260927-run.txt`.

### Continuação do caminho de leitura — 27/09/2026

Uma terceira captura parcial seguiu a execução dentro de `0x06035DC2`. Após o
desvio em `0x06035E10`, o caminho passa por `0x06035E20`, configura o estado 1,
percorre os blocos `0x06035E30–0x06035E88` e retorna ao worker. O caminho não
entra em `0x0603A71A`, que é o ponto observado iniciando transferências do FMV.
No relatório, esse checkpoint continua com 53 hits anteriores; o último é no
passo 8670856, bem antes do pedido de `SNDCOM.ARC`. Depois do enfileiramento, os
pedidos CDC `0x61/0x63` da fila do título continuam em zero e
`0x06053354` permanece em zero.

Isso localiza o bloqueio depois do setup/validação local do handle e antes da
rotina que inicia a transferência de setores. O objeto do atualizador de FMV
`0x0607C000` não volta a executar `0x0603A646` depois do passo 8670417. A próxima
investigação deve seguir como a nova leitura é registrada no atualizador GFS que
alimenta `0x0603A71A`; a carga do mapa e o primeiro setor de `SNDCOM.ARC` ainda
não foram alcançados.

Captura: `build/artifacts/title-gfs-read-path-119m-20260927-run.txt`.

Correção da identificação da fila: o índice usado pelo worker fica em
`0x0605E7F4`; `0x06053350` pertence ao estado consultado pelo laço e não é o
índice da fila. Artefatos: `build/artifacts/title-resolver-to-sector-119m-20260926-run.txt`
e `build/artifacts/title-progress-watch-119m-20260926-run.txt` (captura das
leituras e escritas do estado de progresso).
