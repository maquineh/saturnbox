# Rockman X4 â€” checkpoint do FMV

## Estado anterior (antes da correção do handshake CDC)

- Commit-base: `cfc1a22` â€” `Checkpoint clean FMV and end-of-stream diagnostics`.
- A imagem do FMV estÃ¡ limpa; o defeito de riscos horizontais nÃ£o reapareceu no
  quadro validado em `build/artifacts/video-vdp2-cell-interleaved-3m-r2/frame_000816.png`.
- O problema restante Ã© de tÃ©rmino: a animaÃ§Ã£o permanece no Ãºltimo trecho e o
  fluxo nÃ£o avanÃ§a para a conclusÃ£o do FMV.

## EvidÃªncia do travamento

Nos artefatos `build/artifacts/fmv-checkpoint-3m.txt` e
`build/artifacts/cd-short-window-3m.txt`, o primeiro caminho que
deixa de publicar depois do descritor 26 Ã© o descritor 27 em
`0x060803BC`:

```text
descriptor[0..12] = 0x000785E0, 0x00005610, 0xFFFFFFFF, 0x00000001
requested end     = 0x0007E4E0
stream producer   = 0x0007D000
stream consumer   = 0x00078ED0
```

O alocador percorre `0x0603FC90 -> 0x0603FD04 -> 0x0603FD44`. A comparaÃ§Ã£o
`CMP/HI` em `0x0603FD40` confirma que o produtor ainda nÃ£o alcanÃ§ou o fim
solicitado; `0x0603FD44` apenas contabiliza a espera e o chamador recebe
`R0=0xFFFFFFFF`. Portanto, neste ponto nÃ£o hÃ¡ indicaÃ§Ã£o de erro no decoder de
imagem, na publicaÃ§Ã£o VDP2 ou no DMA.

O serviÃ§o de Ã¡udio tambÃ©m estÃ¡ vivo: o SCSP publica o status em
`0x25A007A0`, e o jogo copia esses estados para `0x0606E608..0x0606E60F`.
O teste controlado de janela curta eliminou as esperas de `0x61`, mas parou no
mesmo descritor 27; essa alteraÃ§Ã£o foi revertida para nÃ£o mascarar a semÃ¢ntica
do CDC.

## PrÃ³ximas etapas

1. Seguir o produtor que deveria mover `stream+84` alÃ©m de `0x7D000`, com foco
   nos caminhos `0x0603EF6C`, `0x0603FFEA`, `0x0603EED6` e no preenchimento GFS
   associado ao descritor 27.
2. Decodificar o significado de `descriptor[2]=0xFFFFFFFF` e
   `descriptor[3]=1`. SÃ³ tratar um final parcial como EOF depois de confirmar
   essa convenÃ§Ã£o no produtor original; nÃ£o forÃ§ar `count`, tamanho de DMA ou
   decoder.
3. Instrumentar a transiÃ§Ã£o que deveria consumir/encerrar o descritor 27 e
   verificar se o callback de conclusÃ£o libera o estado do FMV.
4. Repetir o teste em 3 milhÃµes e depois em 12 milhÃµes de passos, aceitando o
   FMV somente quando o Ã­ndice de descritor avanÃ§ar alÃ©m de 27, os retornos
   repetidos de `0x0603FD44` cessarem e a animaÃ§Ã£o deixar o estado final.

## CritÃ©rio de aceite

O quadro continua sem riscos, o produtor alcanÃ§a o fim do Ãºltimo descritor, o
Ã¡udio deixa de solicitar novos dados, e o dispatcher retorna ao fluxo do jogo
sem alterar artificialmente a base/pitch/formato da imagem ou os limites do
DMA.


## Resultado da investigação — 20/09/2026

O bloqueio no descritor 27 foi corrigido no protocolo CDC. O teste de 12 milhões
de passos consumiu os 139 descritores e iniciou uma nova leitura de disco após
encerrar o FMV. Não foram alterados DMA, decoder, geometria de imagem ou tamanhos
do arquivo para provocar EOF.

### Produtor e causa do bloqueio

`0x0603EE10` avança o produtor (`stream+84`, cursor em `stream+92`).
`0x0603EED6` avança o consumidor (`stream+88`, cursor em `stream+96`);
os rótulos anteriores desses dois pontos estavam invertidos.
O abastecimento segue `0x0603A670 -> 0x06036058`, consulta a conclusão em
`0x06035FCC` e publica os bytes via `0x0603A78A -> 0x0603EE10`.

A reprodução anterior à correção (`build/artifacts/fmv-baseline-current-12m.txt`)
ficou no índice 27, produtor `0x7D000`, consumidor `0x78ED0`, até 12 milhões.
A captura estreita `build/artifacts/fmv-response-race.txt` encontrou a causa:

- Passo 2615994: o jogo envia `0x51` para obter a quantidade de setores.
- Passo 2616000: o tick de subcode substitui a resposta ainda não lida.
- Passo 2616002: o jogo lê `2300,4101,0101,FC17`, um relatório periódico
  contendo FAD, e interpreta `FC17` como quantidade de setores.
- O GFS limita essa quantidade a 17 e calcula/transfere um intervalo incorreto.
  O cálculo parcial antigo permitia posteriormente descartar dados não copiados.

A escrita de CR1 agora protege o pacote até a leitura de CR4 (incluindo aliases).
A produção de setores continua; somente o relatório periódico aguarda o consumo
da resposta. O comando `0x52` responde WAIT quando o intervalo completo ainda não
está disponível e preserva o último tamanho calculado.

Referências de protocolo: [Yabause, cs2.c](https://github.com/Yabause/yabause/blob/master/yabause/src/cs2.c)
(`_command`, escrita de CR1, leitura de CR4 e publicação periódica) e
[Mednafen, cdb.cpp](https://github.com/libretro-mirrors/mednafen-git/blob/master/src/ss/cdb.cpp)
(validação do intervalo de Calculate Actual Size).

### O par FFFFFFFF,1 e o EOF real

A tabela STAB original tem **139 entradas**. Foi extraída do FILM no LBA 129671,
com setores físicos de 2448 bytes, payload em +16 e cabeçalho FILM de `0x8F0`.
A listagem está em `build/artifacts/fmv-original-stab.txt`.

| Índice | Offset | Tamanho | Campo 2 | Campo 3 | Significado |
|---|---|---|---|---|---|
| 0 | `0` | `AC40` | `FFFFFFFF` | `1` | Primeiro áudio |
| 27 | `785E0` | `5610` | `FFFFFFFF` | `1` | Áudio intermediário |
| 132 | `1BD21C` | `1130` | `FFFFFFFF` | `1` | Último áudio |
| 138 | `1D8444` | `4874` | `DA` | `1` | Último vídeo |

`FFFFFFFF,1` é a convenção de amostra de áudio, não um marcador de EOF.
Para vídeo, o quarto campo representa a duração; portanto `1` isoladamente
também não identifica áudio ou fim. Isso coincide com o
[muxer FILM do FFmpeg](https://ffmpeg.org/doxygen/trunk/segafilmenc_8c_source.html).
O código original compara índice e contagem em `0x0604064C..0x06040654`,
aguarda os consumidores e grava estado 5 em `0x0604071E..0x06040722`.
O fim exigido pelo último descritor é `0x8F0 + 0x1D8444 + 0x4874 = 0x1DD5A8`.

### Validação

- Build Release e suíte CTest passaram (86 casos no executável de testes).
- Regressões cobrem tick entre envio/leitura da resposta, tick durante a escrita
  do comando, avanço de setores durante a resposta pendente e alias de CR4.
- Outra regressão verifica WAIT de `0x52`, preservação do resultado anterior,
  transferência dos dois setores sem perda e offsets/contagens `FFFF`.
- Primeira execução corrigida: `build/artifacts/fmv-handshake-12m.txt`.
  Índices em 3/5/7/8/9 milhões: 32/62/105/130/139.
- Repetição limpa: `build/artifacts/fmv-handshake-verified-12m.txt`, também
  completou 12000000 passos. Os índices, hashes dos quadros e contadores PCM
  coincidiram com a primeira execução; estado 5 e índice 139 de 9 milhões em diante.
- Quadros 915, 1491, 1936 e 2103 inspecionados em
  `build/artifacts/fmv-handshake-verified-frames`: animação limpa, logo CAPCOM
  e fade de saída, sem os riscos horizontais anteriores.
- Produtor final `0x1DD800`, consumidor `0x1DD5A8`: todo o FILM disponível e
  consumido, restando apenas padding de setor.
- Último consumo de áudio em 8167882 passos; 321928 bytes por canal publicados.
  O contador de atualizações PCM fica em 134 de 9 a 12 milhões, status PCM 6.
- O callback GFS do FMV deixa de executar após aproximadamente 8671931 passos.
  Em 8678395, há novo PLAY com FAD 654 e 46 setores: o fluxo do jogo prosseguiu
  para outro carregamento, em vez de repetir o descritor final do FMV.

O carregamento seguinte apresenta tela apagada e DMA SCU pendente
(`0x25818000 -> 0x25E60000`, 2048 bytes). A saída do FMV está demonstrada;
a chegada à tela de título ainda não está validada. Esse DMA não foi forçado.


Reprodução em PowerShell (a imagem de disco é local e não faz parte do repositório):

## Resultado do carregamento seguinte - 20/09/2026

A ISO confirma os dois CPK do video: `LOGO.CPK` comeca no LBA 129671 (FAD
129821), com 139 entradas STAB, e `OP.CPK` comeca no LBA 130626 (FAD
130776), com 1708 entradas STAB. O primeiro e o FMV que chega ao logo CAPCOM;
o segundo permanece disponivel na imagem para o fluxo de abertura.

O DMA pendente do segundo carregamento era uma falsa espera: o jogo limpa
`HIRQ.DRDY` antes do GO manual, embora os bytes ainda estejam no FIFO. O SCU
agora consulta `fifoDataAvailable()` (transferencia ativa e offset ainda dentro
do payload), sem reativar HIRQ, alterar o tamanho ou fabricar dados. A regressao
`transfersCdFifoThroughScuDma` cobre esse intervalo exato.

No teste corrigido de 12 milhoes de passos, o DMA termina com `2048/2048` bytes,
`count=0`, e nao ha descritor CD pendente. O executor tambem materializa o
retorno da chamada BIOS de pos-FMV em `0x0022750C`, presente no `0.BIN` e
necessario para nao parar no alvo dinamico antes do titulo. O estado final fica
com `display=on`, VDP1 ativo (43 listas, 18 sprites) e VDP2 habilitado; os
artefatos estao em `build/artifacts/fmv-postbios-probe-12m.txt` e
`build/artifacts/fmv-op-long-fixed-20260920.txt`.

O caminho automatico executado pelo `0.BIN` termina no titulo depois de `PLAY`
de LOGO e das faixas de dados auxiliares; ele nao emite um `PLAY` para FAD
130776 durante essa sessao. Portanto a presenca e a localizacao de `OP.CPK`
estao confirmadas na ISO, mas a segunda animacao ainda nao foi atravessada pelo
fluxo de entrada sem inventar um comando de CD.

```powershell
& build/tools/saturn-sh2/Release/saturn-sh2.exe boot build/rockman/aip.bin `
  --aip-base 0x06002E40 --ip build/rockman/ip.bin --ip-base 0x06002000 `
  --program build/rockman/0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf' --disc-sector-size 2448 `
  --continue-after-execdmp --vblank-blocks 10000 --trace-game --steps 12000000 `
  --frame-dir build/artifacts/fmv-handshake-verified-frames --frame-changes-only `
  > build/artifacts/fmv-handshake-verified-12m.txt 2>&1
```


## X4LOW e a janela preta do titulo - 20/09/2026

O alvo `0x0022750C` chamado por `0.BIN` nao e uma BIOS vazia: ele e o deslocamento
`+0x750C` do executavel `X4LOW.X4`, carregado em `0x00220000`. A entrada extraida
da ISO tem 565247 bytes e comeca com codigo SH-2 valido (`D508 E460 ...`). O
resolver estatico nao enxerga esse segmento porque ele so aparece depois da DMA;
substituir a chamada por um retorno faz o teste prosseguir, mas pula o codigo que
pode iniciar a proxima animacao. Uma tentativa de baixar o CFG dinamico durante a
execucao ultrapassou 13 GB de memoria antes de produzir um novo `PLAY`, por isso
foi interrompida e nao foi incorporada ao fluxo estavel.

O fluxo estavel agora trata somente essa entrada como uma rotina limitada: le os
literais de destino `0x25C7FF00` e origem `0x00290A6C` no `X4LOW.X4` e repete as
32 iteracoes de tres palavras que a rotina SH-2 executa antes do `RTS`. O teste de
12 milhoes de passos terminou com 4011 blocos baixados sob demanda e sem o pico
de memoria do CFG completo. A reducao nao cria um `PLAY` artificial: a sequencia
continua com oito `PLAY` e ainda nao alcanca `FAD 130776`.

A amostragem densa do fim do FMV separa as duas situacoes. No frame 2190 ainda ha
69081 pixels nao pretos do FMV. A partir do frame 2200 o VDP2 desliga o display
(`TVMD=0x0010`) e a composicao fica em zero. No frame 2560 o jogo volta a ligar o
display (`TVMD=0x8010`, `BGON=0x000F`), preenche 7002 palavras de VRAM e 706 bytes
de CRAM, mas `NBG0` e a composicao continuam sem pixels. Isso confirma a troca de
estado do VDP2, mas nao confirma a chegada visual ao titulo; a tela preta ainda
nao deve ser chamada de titulo carregado.
