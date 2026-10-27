# Rockman X4: cadeia do primeiro frame preto com display reativado

Captura de 20/09/2026, frame **2469**, durante a execução de 12 milhões de
blocos. O gatilho exige imagem visível anterior, display desligado e depois
`TVMD=0x8010` com RGB zero em todos os pixels. Não confunde o preto inicial
do boot com a transição pós-FMV. Não altera o fluxo do jogo ou a renderização.

## Evidência

| Etapa | Observação do renderizador atual |
|---|---|
| Registradores | `PNCN0=0`, `CHCTLA=0x0101`, `PLSZ=0x15`, `MPOFN=0x111`, `MPABN0=MPCDN0=0x3030` |
| Escritas VRAM | 7002 **bytes** não nulos em `0x53080–0x60FC6` |
| Mapa NBG0 | Endereço calculado inicial `0x1C0000`; após máscara de VRAM, `0x40000` |
| Nomes de padrão | 280 entradas amostradas, em offsets `0x40000–0x406CC` (não é um intervalo de leitura contínuo); ambas as palavras de todas as entradas são zero |
| Histórico do mapa | Todas as 42 regiões de 32 bytes consultadas tiveram última escrita no contador de frame 2192; o dump atual está zerado nessas entradas |
| Caracteres | Número zero, leituras em `0x00000–0x0007F`, todas zeradas; última escrita das quatro regiões no frame 2192 |
| Cor | Paleta zero, índice zero, leitura de `CRAM[0]=0`; há 706 bytes não nulos em outras posições da CRAM (`0x2–0x2FF`) |
| Janela e prioridade | `WCTLA=0`, todos os 71680 pixels permitidos; `PRINA=0x0505`, prioridade NBG0=5, programação de prioridade ativa |
| Composição | Zero pixels RGB não pretos na saída decodificada NBG0, camada NBG0, VDP2 e frame final |
| VDP1 | Banco exibido 1 está preto; banco 0 contém 7665 pixels não pretos. A captura não atribui a causa da seleção de banco |

O primeiro ponto observável da cadeia NBG0 é a leitura de nomes zero. A
camada já chega preta à composição: prioridade ou janela não são necessárias
para explicar esse resultado. Isso **não prova mapa ausente no endereço que
o hardware deveria usar**. Ainda é necessário validar a fórmula de endereço
para nomes de duas palavras, caracteres 16x16 e planos de duas páginas.
Também não comprova que a CRAM global esteja incorreta.

## Artefatos e limites

Diretório: `build/artifacts/first-black-chain-20260920/`.

- `frame_002469_cells.csv`: todos os 71680 pixels, com endereço/valor do nome,
  número/endereço do caractere, palavra lida, endereço/valor CRAM, cor
  decodificada, janela, NBG0, VDP2 e frame final. Endereços no CSV são valores
  calculados **antes** das máscaras (`VRAM & 0x7FFFF`, `CRAM & 0xFFE`).
- `frame_002469_writes.csv`: contagem de escritas de barramento por região de
  32 bytes e contador de frame na última escrita. Não inclui PC, origem DMA,
  valores históricos nem timestamp de instrução; não é histórico por byte.
- `frame_002469_{vram,cram,registers}.bin`: dumps brutos completos.
- `frame_002469_{nbg0,vdp2,final,vdp1_bank0,vdp1_bank1,back}.ppm`: imagens.
- `summary.json`: cruzamento dos CSVs e dumps, reproduzível com
  `python build/artifacts/analyze-black-chain.py`.
- `jev-request.json` / `jev-response.json`: consulta e resposta sem credenciais.
- Log: `build/artifacts/first-black-chain-20260920-trace.txt`.

O TXT desta primeira captura ainda contém campos legados `NBG0.base`,
`width`, `bits_per_pixel` e `line[*]` calculados como bitmap; não são válidos
para este modo. Use o CSV de células. O writer foi corrigido para omitir
esses campos em novas capturas de modo cell.

As leituras vêm do mesmo caminho `composeVdp2Cell` que renderiza a camada,
reexecutado sem mutação na fronteira de apresentação. Não são validação
independente das regras de endereçamento Saturn. Os dados brutos permitem
essa comparação sem nova execução.

## Avaliação JEV

Consulta por TypeSafe, modelo `jev-1.13.0`, com o resumo da captura e opções
explícitas incluindo evidência insuficiente. Escolheu mapa lido zero como
primeiro ponto observável (probabilidade 0,92). Escolheu que a captura não
distingue mapa não carregado de endereço incorreto (1,00), que CRAM[0]=0 não
comprova defeito global de CRAM (1,00) e que o próximo teste é validar a
fórmula de endereço contra especificação (1,00). São escolhas entre hipóteses
fornecidas, não provas adicionais nem percentuais de correção do emulador.

## Reprodução

```powershell
& build/tools/saturn-sh2/Release/saturn-sh2.exe boot build/rockman/aip.bin `
  --aip-base 0x06002E40 --ip build/rockman/ip.bin --ip-base 0x06002000 `
  --program build/rockman/0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf' --disc-sector-size 2448 `
  --continue-after-execdmp --vblank-blocks 10000 --trace-game --steps 12000000 `
  --black-frame-probe build/artifacts/first-black-chain-20260920 `
  > build/artifacts/first-black-chain-20260920-trace.txt 2>&1
```

Preservar `--trace-game`: a tentativa sem essa opção apresentou crescimento
de memória e foi interrompida antes de gerar captura. A causa dessa diferença
não foi investigada neste trabalho. A execução com a opção reproduziu os
hashes do FMV registrados no checkpoint.

Validação: build Release, CTest e teste do gatilho (ignorar preto inicial,
capturar apenas após visible/off/on, capturar uma vez, registrar leituras e
escritas que cruzam fronteira de 32 bytes). Nenhuma correção funcional de
VDP2, CD, DMA ou fluxo do jogo foi aplicada.

## Validação independente da base do mapa

O teste seguinte confirmou **dois fatos simultâneos**: a base calculada pelo
renderizador está incorreta para caracteres 16x16, e o mapa no endereço
correto também está zerado no frame 2469. Portanto, corrigir apenas essa
base não faria NBG0 aparecer neste snapshot.

A [tabela 4.8 do manual Sega](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_80.htm)
define o multiplicador segundo o tamanho do plano, do nome e do caractere.
Para `PNCN0=0` (duas palavras), `CHCTLA=0x0101` (NBG0 16x16, 4 bpp) e
`PLSZ=0x15` (NBG0 com plano de duas páginas horizontais), cada página ocupa
`0x1000` bytes e cada plano `0x2000` bytes. A capacidade da página também é
confirmada pela [tabela 4.4](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_60.htm).

```text
MPOFN.N0 = 1
MPABN0.A = 0x30
valor combinado = (1 << 6) | 0x30 = 0x70

Manual, VRAM de 4 Mbit (bit superior usado desconsiderado):
  ((0x70 & 0x7E) >> 1) * 0x2000 = 0x70000

Renderizador atual, multiplicador de caracteres 8x8:
  (0x70 >> 1) * 0x8000 = 0x1C0000
  0x1C0000 & 0x7FFFF = 0x40000
```

`CalcPlaneAddr` no [Yabause](https://github.com/Yabause/yabause/blob/master/yabause/src/vidshared.h)
fornece uma verificação adicional: no ramo de nomes de duas palavras e
caracteres 16x16, usa o tamanho de página `0x1000`, resultando na mesma base.
O cálculo foi derivado do manual; nenhum código dessa implementação foi
copiado para o renderizador.

### Resultado no dump

- Os planos A/B/C/D de NBG0 têm o mesmo registro de mapa e a mesma base
  `0x70000`. A faixa do plano de duas páginas é `0x70000–0x71FFF`.
- Todos os 8192 bytes dessa faixa são zero. As 280 entradas visíveis ficam
  em `0x70000–0x706CC` (endereços esparsos), todas com ambas as palavras zero.
- Mudar somente a base conserva a seleção de caractere zero, paleta zero e
  pixels pretos neste snapshot; não foi necessário simular novo boot para
  comparar esses bytes.
- As 256 regiões de 32 bytes da faixa correta **receberam escritas**. Todas
  registram frame 2453 como última escrita. A faixa errada tinha última
  escrita no frame 2192; seguir somente essa faixa perderia o produtor recente.
- Não é possível inferir desses metadados se houve limpeza de um mapa
  preenchido ou se as escritas já receberam dados zero. Também não demonstram
  que o mapa permanecerá zero em frames futuros.

### Reprodução e JEV

```powershell
python tools/diagnostics/validate_nbg0_map_capture.py `
  build/artifacts/first-black-chain-20260920
```

O script valida o modo e os tamanhos dos dumps, confere as 71680 leituras
originais, calcula os endereços independentemente e verifica as 280 entradas
corretas, o plano inteiro e seus metadados de escrita. A execução passou;
`map-address-validation.json` inclui resultados, limites e hashes SHA-256
dos três dumps. O teste é específico deste modo/frame, não um validador
completo de zoom, navegação entre páginas, cores ou endereçamento de caracteres.

O JEV (`jev-1.13.0`) recebeu esse resultado e escolheu a hipótese de ambos
os fatos (probabilidade 1,00), a interpretação de que houve escrita sem
causa determinada (0,99), e rastrear os produtores da faixa correta (0,89).
Requisição e resposta: `jev-map-validation-{request,response}.json` no
diretório da captura. Essas probabilidades comparam alternativas fornecidas;
o suporte técnico é o manual e a verificação do dump.

Este passo adicionou o validador e registrou o defeito; não modificou a
fórmula do renderizador. O próximo trace deve capturar PC/chamador, valores
e origem DMA das escritas em `0x25E70000–0x25E71FFF`, especialmente perto do
frame 2453, e acompanhar as atualizações posteriores.

O trace foi executado e está detalhado em
[ROCKMAN-X4-NBG0-MAP-PRODUCER.md](ROCKMAN-X4-NBG0-MAP-PRODUCER.md). Ele
encontrou 18476 escritas na faixa: 14380 por CPU, 4096 por SCU-DMA, 18433
zeros e 43 valores não nulos. O registro do valor anterior mostra 86 mudanças:
43 células passam de zero para não zero no bloco `0x0601667C` e as mesmas 43
voltam a zero na limpeza posterior do bloco `0x06034B5C`; os demais zeros são
reescritas sobre células já zeradas. A atribuição DMA usa passo, bloco e
chamador do contexto da transferência, portanto o DMA de limpeza não é apenas
uma inferência baseada em sobreposição de endereços.

O primeiro trace de leitura, anterior à correção da fórmula, está em
[ROCKMAN-X4-NBG0-MAP-READ.md](ROCKMAN-X4-NBG0-MAP-READ.md). Ele mostra que as
43 palavras ficam presentes enquanto NBG0 está em bitmap (`CHCTLA=0x0042`),
mas são apagadas no passo `8673634` antes de NBG0 mudar para célula
(`CHCTLA=0x0101`). Como `VCSTA=0x70000` enquanto NBG0 está em bitmap, essas
43 palavras podem ser a tabela de vertical-cell-scroll, e não nomes de mapa.
O trace inicial leu `0x40000–0x406CE`; a captura após corrigir o endereço lê a
faixa esperada `0x70000–0x706CE` e ainda encontra tudo zerado. Os dois boots de
26/09/2026 e os limites dessa conclusão estão em
[ROCKMAN-X4-NBG0-MAP-READ.md](ROCKMAN-X4-NBG0-MAP-READ.md).
