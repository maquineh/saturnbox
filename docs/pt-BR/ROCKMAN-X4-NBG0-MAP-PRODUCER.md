# Escritas na faixa NBG0 `0x70000–0x71FFF`

Continuação e correções de interpretação: [caminho do mapa do título](../ROCKMAN-X4-TITLE-MAP-FIX.md).
Este relatório mantém a captura histórica; `cpu-store` nas capturas antigas
também pode significar DMA cujo descritor excedeu o limite do histórico.

Esta captura rastreia cada escrita na faixa física calculada como plano NBG0
para o modo do primeiro frame preto: `0x25E70000–0x25E71FFF` (offsets VDP2
`0x70000–0x71FFF`). A mesma região servia como tabela de vertical-cell-scroll
quando NBG0 estava em bitmap. O trace registra passo SH-2, bloco, chamador,
registradores, valor anterior, valor novo e a transferência SCU-DMA que coincide
com o contexto da escrita.

## Captura

```powershell
& build/tools/saturn-sh2/Release/saturn-sh2.exe boot build/rockman/aip.bin `
  --aip-base 0x06002E40 --ip build/rockman/ip.bin --ip-base 0x06002000 `
  --program build/rockman/0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf' --disc-sector-size 2448 `
  --continue-after-execdmp --vblank-blocks 10000 --trace-game --steps 12000000 `
  --vdp2-map-trace build/artifacts/map-producer-trace-20260920-e
```

Os arquivos são `vdp2-map-producer.csv` e
`vdp2-map-dma-transfers.csv` em
`build/artifacts/map-producer-trace-20260920-e/`. O CSV foi gerado depois de
associar o contexto SH-2 ativo ao registro da transferência; um DMA só é
atribuído quando passo, bloco e chamador coincidem. Isso evita classificar uma
escrita de CPU como parte de um DMA anterior que cobria toda a VRAM.

Cada linha também contém `previous`, o valor lido imediatamente antes da
escrita, e `changed`. Assim é possível separar uma escrita zero que alterou
uma célula de uma reescrita de zero sobre uma célula que já estava zerada.

## Resultado

| produtor (bloco / chamador) | passos | escritas | valores escritos | interpretação |
| --- | ---: | ---: | --- | --- |
| CPU, `0x06002FD4` / `0x06002FD4` | `106945–108992` | 2048 × 32 bits | todos `0x00000000` | todos já eram zero |
| SCU DMA canal 0, contexto `0x06034B5C` / `0x0F000018` | `502806` | 4096 × 16 bits | todos `0x0000` | todos já eram zero |
| CPU, `0x0601667C` / `0x06016672` e `0x0601667C` | `515553–515574` | 44 | `0x0000`, depois `0x0400` até `0xAC00` | 43 mudanças de zero para não zero |
| CPU, `0x06034B5C` / `0x0F000018` | `8673634` | 4096 × 16 bits | todos `0x0000` | 43 zeros apagaram valores não nulos |
| CPU, `0x06012DBC` / `0x06012DBA` | `8675512` e `11257939` | 8192 × 16 bits | todos `0x0000` | todos já eram zero |

No total foram 18476 escritas: 14380 por CPU e 4096 por DMA. 18433 valores
foram zero e apenas 43 foram não nulos. Houve 86 mudanças efetivas: 43
células passaram de zero para não zero na carga e as mesmas 43 passaram de
`0x0400–0xAC00` para zero na limpeza do passo `8673634`. Os outros 18390
zeros foram escritos sobre células que já estavam zeradas.

Os 43 não nulos pertencem ao bloco `0x0601667C`; o primeiro valor é `0x0400`
em `0x25E7000C`, seguido por duas entradas por passo, de `0x0800` até
`0xAC00`, em offsets até `0x7015C`. A limpeza que os remove ocorre em
offsets de células de `0x7000E` a `0x7015E`, com `previous` registrando cada
valor não nulo antes de gravar `0x0000`.

O DMA correspondente é o índice 4 do histórico, canal 0:

```text
source      = 0x06009F6C
destination = 0x25E00000
count       = 0x80000 bytes
add         = 0x1
mode        = 0x107
map writes  = 4096 halfwords, todos 0x0000
context     = step 502806, block 0x06034B5C, caller 0x0F000018
```

Com `add=0x1`, o destino avança em unidades de 2 bytes e a origem permanece
fixa neste modo. Portanto, para a faixa NBG0, o evento é um preenchimento por
uma palavra fonte zero, não uma cópia de dados de mapa variáveis. A origem não
é o FIFO do CD (`source=0x06009F6C`).

## Interpretação

A captura prova que a CPU escreve 43 valores não nulos na região enquanto
NBG0 está em bitmap e depois zera essas mesmas posições no passo `8673634`.
O DMA e as outras limpezas reescrevem zeros sobre posições já zeradas. Como
`VCSTA=0x70000` durante o bitmap, essas escritas podem ser dados de scroll; o
trace não prova que a CPU carregou um mapa de nomes e depois o apagou. A captura
de leitura posterior, com o endereço corrigido, mostra a faixa zerada no modo
célula. Ainda falta identificar a rotina ou recurso que deveria preencher o
mapa de nomes do título.

Validação: build Release e CTest passaram (`1/1`).
