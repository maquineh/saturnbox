# Recompilação estática observada — 31/10/2026

O `saturn-aot` gerou shards C++ para as raízes observadas no replay de jogador
de 27/10. A geração leu três segmentos SH-2 extraídos e a lista de alvos
dinâmicos daquele replay; não iniciou a ROM, o emulador ou testes.

## Entradas verificadas

| Arquivo | Bytes | SHA-256 |
|---|---:|---|
| `build/rockman/ip.bin` | 6.144 | `67172e065c7c31ea55d02ea8cbe95fc11e564b4ae8f870bc7b66f35ada6ed615` |
| `build/rockman/0.BIN` | 415.320 | `0342da7e0583ad9b869f17c7247d5753f7e771a76d8041573aef3730ab5da10f` |
| `build/rockman/X4LOW.X4` | 565.247 | `901af9fe7bd256e8771a46badcdc9895d4128f368a5eb0a1823e492d3979d2e7` |
| `build/rockman/player-route-20261027-run1-entries.txt` | 97.116 | `e997ce58ff0962999c492e995108950213779a4ccc74c4b633cb9ffcb6329b08` |

O nome de arquivo `X4LOW.X4` não determina seu tamanho; esta extração tem
565.247 bytes. Relatórios de tamanho diferentes descrevem outra cópia ou
extração.

## Resultado AOT

Saída em `build/rockman/native-route-20261031-aot-shards/`:

- 1 variante de imagem, 3 segmentos e 8.093 entradas;
- 12.719 blocos encontrados, baixados e emitidos;
- 215.106 instruções SIR emitidas;
- 0 blocos ignorados e 0 falhas de lifting;
- 12 shards por banco de endereço, mais registro e lista CMake.

Os shards foram ligados ao `saturn-sh2.exe` Release em
`build/native-route-20261031-compile/`. O build MSVC 19.44 foi concluído com
sucesso. `SATURNBOX_BUILD_TESTS=OFF` e `SATURNBOX_VULKAN=OFF`; não houve
execução do binário produzido.

```powershell
& .\build\tools\saturn-aot\Release\saturn-aot.exe `
  --segment 0x06002000 .\build\rockman\ip.bin `
  --segment 0x06009000 .\build\rockman\0.BIN `
  --segment 0x00220000 .\build\rockman\X4LOW.X4 `
  --entry 0x06002E40 --entry 0x06010000 `
  --entry 0x00220700 --entry 0x00220800 `
  --entry-list .\build\rockman\player-route-20261027-run1-entries.txt `
  --output-dir .\build\rockman\native-route-20261031-aot-shards

cmake -S . -B build/native-route-20261031-compile `
  -DSATURNBOX_BUILD_TESTS=OFF -DSATURNBOX_VULKAN=OFF `
  -DSATURNBOX_STATIC_MODULE_DIR="H:/projetos/saturnbox/build/rockman/native-route-20261031-aot-shards"
cmake --build build/native-route-20261031-compile --config Release `
  --target saturn-sh2 --parallel 8
```

## Consolidação de rotas anteriores

Depois da geração isolada, consolidei cinco módulos AOT: abertura/título
(`rockman_static.cpp`), uma rota realtime até o stream ST04, o módulo de
jogador de 27/10, o novo módulo de 31/10 e o módulo histórico
`rockman_player_stage_observed.cpp` do checkout saturn-recomp. O agregador
preserva blocos com o mesmo endereço e assinatura SIR diferente, e remove
duplicatas iguais.

- 46.339 entradas somadas nos cinco módulos de origem;
- 17.300 pares únicos endereço/assinatura em 15.411 endereços;
- 1.889 endereços têm mais de uma assinatura no corpus combinado;
- o módulo histórico acrescentou 2.715 pares em 2.715 endereços;
- os 10.976 pares sobrepostos têm corpos C++ gerados equivalentes após
  normalizar nomes de função e temporários; não houve divergência nesse cotejo;
- o executável Release combinado compilou com sucesso; 7.179.264 bytes,
  SHA-256 `7e2c666f8ad4198ab36524e6df58d0f972d302ce57579a16f7af317372744e61`.

A contagem foi feita lendo as tabelas C++ emitidas; ela não observa qual
variante o runtime escolheria. A comparação dos corpos é estática e não valida
comportamento do jogo. O manifesto de fusão guarda hashes de entrada e saída,
e `coverage-report.json` registra a contagem, a auditoria de compatibilidade e
o hash do executável. O build usou `SATURNBOX_BUILD_TESTS=OFF`; o binário não
foi executado.

```powershell
Copy-Item H:/projetos/saturn-recomp/build/rockman/rockman_player_stage_observed.cpp `
  ./build/rockman/rockman_player_stage_observed_compat.cpp
$compatPath = './build/rockman/rockman_player_stage_observed_compat.cpp'
$compat = [IO.File]::ReadAllText($compatPath)
$compat = $compat.Replace('saturn_recomp_static_blocks', 'saturnbox_static_blocks')
[IO.File]::WriteAllText($compatPath, $compat, [Text.UTF8Encoding]::new($false))

& .\tools\diagnostics\merge_x4_aot.ps1 `
  -Modules @('.\build\rockman\rockman_static.cpp', `
    '.\build\perf\trained-realtime-stage\module-1.cpp', `
    '.\build\rockman\player-route-20261027-run1-module.cpp', `
    '.\build\rockman\native-route-20261031-module.cpp', `
    '.\build\rockman\rockman_player_stage_observed_compat.cpp') `
  -OutputDirectory .\build\rockman\native-route-20261031-combined-v2

cmake -S . -B build/native-route-20261031-combined-v2-compile `
  -DSATURNBOX_BUILD_TESTS=OFF -DSATURNBOX_VULKAN=OFF `
  -DSATURNBOX_STATIC_MODULE_DIR="H:/projetos/saturnbox/build/rockman/native-route-20261031-combined-v2"
cmake --build build/native-route-20261031-combined-v2-compile --config Release `
  --target saturn-sh2 --parallel 8
```

O módulo adicional foi copiado do checkout `H:/projetos/saturn-recomp/` para
o diretório de build, com apenas o nome do getter de registro adaptado. Seus
13.691 registros vêm de uma rota histórica com 398 blocos ignorados; isso não
prova cobertura completa do estágio ou validação de movimento do jogador.

## Limites de cobertura

Este resultado recompila para C++ blocos alcançáveis pelas entradas fornecidas
nos três segmentos. A entrada observada cobre uma rota de jogador, não todas as
fases, finais, estados condicionais, overlays ou caminhos indiretos. O módulo
depende de `saturn_sir`, `MachineState` e dos serviços SaturnBox; não é um
executável independente do jogo nem um port concluído. Endereços sem código
nesse corpus continuam sujeitos à descoberta dinâmica/JIT.

As opções de janela implementadas no mesmo runtime configuram a apresentação
host: `--window-size 1920x1080`, `--integer-scale` e `--smooth-scale`. A
proporção do quadro e a resolução VDP1/VDP2 permanecem nativas; barras laterais
preenchem telas mais largas. Para revelar mais cenário seria necessário
modificar a câmera e validar os caminhos do jogo.
