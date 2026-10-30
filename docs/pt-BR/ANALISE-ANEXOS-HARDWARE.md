# Análise dos textos técnicos sobre o Saturn

Revisão dos textos enviados em 2026-10-03, comparados com o código e os testes
atuais. A conclusão é aproveitar os conceitos como referências e checklist;
vários exemplos descrevem partes que já existem, enquanto outros propõem
backends ou precisão de hardware que ainda não fazem parte deste recorte.

## O que já está implementado

| Área | Estado no projeto |
| --- | --- |
| SH-2 | Decoder, descoberta de CFG, lowering para SIR-L0, interpretador, comparação diferencial e resolução runtime de alvos indiretos. Delay slots ficam explícitos no CFG/SIR. Ainda não há backend que gere C/C++ nativo. |
| CD e ISO 9660 | `CdRuntime` lê imagens 2048/2352/2448, atende comandos/HIRQ/FIFO e encontra arquivos ISO por caminho. O HLE não simula servo, microprocessador SH-1 nem temporização física do drive. |
| SCU DMA | Três canais com registradores, passos de endereço, fatores observados, espera por dados do FIFO e histórico de transferências. A cópia pode ser síncrona no HLE; a conclusão e a interrupção são observadas no limite do scheduler. Isso não equivale a um modelo geral de ciclos do barramento. |
| SMPC | Interface de registradores byte-only, reset básico, `INTBACK`, handshake de status/periféricos e dois pads digitais ativos em zero. RTC e SMEM ainda retornam zero. |
| VDP1/VDP2 | Memórias e registradores MMIO, percurso de comandos VDP1, framebuffer software e composição de BACK/NBG0/NBG1, inclusive mapas PNT/células e parte das janelas. Não há backend OpenGL/Vulkan. |
| SCSP | HLE do mailbox observado e subconjunto PCM Start/Stop, mixer estéreo e frames de áudio. Não executa o 68K nem reproduz integralmente envelopes, DSP, sequências ou o chip SCSP. |
| Diagnóstico | `--trace`, `--trace-game`, contexto de bloco/MMIO, traces focados de mapas e snapshots de vídeo/áudio. O recorte usa evidência direcionada em vez de registrar indiscriminadamente cada passo. |

## O que os textos acrescentam ao trabalho de vídeo

O manual da Sega confirma a distinção que faltava na leitura dos comandos VDP1:

- shape `0` é sprite fixo: usa XA/YA, CMDSIZE e os bits de direção; as outras
  coordenadas são ignoradas;
- shape `1` com zoom point `0` usa os vértices A e C como os cantos da área e
  a ordem deles também pode inverter a imagem;
- zoom points diferentes de zero e shape `2` ainda precisam de implementação
  específica. O shape `2` atual cobre o retângulo delimitador, não a
  transformação quadrilateral do hardware.

Também conferi os campos de `CMDSIZE`: X ocupa os bits 13–8 e cada unidade
representa 8 pixels; Y ocupa os bits 7–0 e é um valor direto de 1 a 255.
Portanto, `0x0210` significa 16×16 pixels. A tabela HTML da especificação
parece anexar “/8” às duas dimensões, mas a prosa do próprio manual restringe
a divisão à largura, e o overview confirma a altura vertical máxima de 255.
Referências: [comando de sprite retangular, manual VDP1 §7.5](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p07_51.htm) e
[limites dos sprites, overview Sega §3.3](https://www.infochunk.com/saturn/segahtml_en/hard/intr/hon/p03_30.htm).

Essa leitura corrige o caso observado no Rockman X4: a lista capturada tinha
74 sprites ativos shape `0`. Além do renderer antigo usar B/C/D para esticar as
texturas, a primeira captura revelou a inversão dos campos X/Y de `CMDSIZE`:
`0x0210` foi interpretado como 128×2, embora seja uma célula 16×16. O renderer
e o relatório de comandos agora usam os campos de hardware; shape `0` usa XA/YA
e shape `1` com zoom point `0` usa A/C. A captura antiga de pixels não serve
para avaliar a composição. O replay corrigido e a comparação por pixel entre
VDP1 e VDP2 estão registrados em
[`ROCKMAN-X4-TITLE-MAP-FIX.md`](../ROCKMAN-X4-TITLE-MAP-FIX.md#per-pixel-vdp1vdp2-priority-composition-2026-10-04).

Referências primárias: [VDP1 User's Manual, 7.4 — Fixed sprite](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p07_40.htm) e
[7.5 — Rectangle sprite](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p07_50.htm).

A proposta de traduzir os chips diretamente para shaders pode ser uma direção
de backend futuro. Agora o renderer lê os arrays de memória do runtime e
compõe pixels no host; por isso, dirty pages e staging buffers não aceleram o
caminho atual. Se houver backend GPU, as escritas MMIO/DMA serão o ponto certo
para marcar páginas e enviar deltas, preservando a ordem de visibilidade.

## Pontos que exigem correção ou limite explícito

- **BRAM:** o texto indica `0x20000000` como base. O mapa Sega coloca a janela
  em `0x00180000..0x0018FFFF`; no alias cache-bypass usado pelo projeto ela
  fica em `0x20180000..0x2018FFFF`. A região expõe 64 KiB de endereços para
  32 KiB físicos; a implementação de MAME também ignora os bytes pares e
  armazena os ímpares compactados. Ainda não existe RAM de backup persistida
  em arquivo neste runtime. Referências: [mapa do SCU da Sega](https://antime.kapsi.fi/sega/files/ST-097-R5-072694.pdf) e [implementação de referência do MAME](https://github.com/mamedev/mame/blob/master/src/mame/sega/saturn.cpp).
- **Endianness:** memória convidada e registradores já usam operações
  explícitas big-endian. O tradutor não deve ler structs Saturn por casts de
  ponteiro do host nem espalhar `bswap`; deve usar as primitivas de memória do
  runtime.
- **BIOS/TRAPA:** SIR representa `TRAPA` como evento arquitetural e
  `BootRomServices` implementa os serviços de boot necessários ao corpus. Isso
  não é uma BIOS completa nem justifica interceptar indiscriminadamente todos
  os vetores.
- **Ghidra:** `DecompInterface` é útil para exportar pseudocódigo e orientar a
  nomeação/análise manual. Esse C é uma reconstrução do decompilador, não o
  código fonte original nem saída compilável do recompilador. Uma lista de
  funções também não deve alimentar automaticamente o dispatcher: símbolos
  dependem de código alcançável e de limites de função validados. No corpus
  local, `0.BIN` é carregado em `0x06009000` e entra em `0x06010000`; o exemplo
  genérico `0x06004000` não deve ser aplicado sem conferir o mapa da imagem.
  Uma tabela de símbolos externa pode, sim, enriquecer traces e relatórios por
  endereço.
- **Timing:** o scheduler reproduz eventos em passos determinísticos de bloco
  configuráveis. Não equivale a 263 fatias de scanline com contagem de ciclos
  validada; o launcher faz pacing de apresentação, separado da semântica do
  SH-2. Manter essa separação evita misturar `sleep`/barreiras de threads ao
  interpretador de referência.
- **Dois SH-2 e áudio:** as flags de campo do X4 são publicadas pelos callbacks
  guest de VBlank; um fallback legado só atende harnesses sem VBlank periódico.
  Ainda não há uma segunda CPU SH-2 executando código concorrente. O
  PCM observado tampouco confirma execução de música CD-DA ou emulação completa
  do SCSP/68K. Esses são marcos de compatibilidade futuros, não pré-requisitos
  para corrigir a composição atual do Rockman.
- **DMA:** o exemplo genérico de `memcpy` instantâneo é insuficiente como
  semântica universal. Origem/destino, passos, FIFO, fator de ativação, dados
  observáveis antes da conclusão e entrega da interrupção importam; o runtime
  atual mantém esses eventos observáveis no scheduler.

O [script Ghidra de exportação](https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/DecompInterface.java)
usa uma interface real para solicitar pseudocódigo C e pode ser usado como
ferramenta externa de pesquisa. Para tornar esse script mais robusto, a
inicialização e o loop devem ficar protegidos por `try/finally` para sempre
chamar `dispose()`, e a escrita deve declarar UTF-8. Mesmo quando a
descompilação termina sem erro, o texto pode depender de tipos, globais e
funções auxiliares do programa; não é uma unidade C autônoma. O primeiro uso
recomendado é inspecionar funções e produzir um mapa revisado de nomes/endereço
para traces. Cabeçalhos extraídos da primeira linha do pseudocódigo e
dispatchers montados a partir de toda função detectada não substituem o CFG, o
resolver runtime nem os contratos do SIR.

## Próximos passos recomendados

1. Inspecionar as fontes e cores dos sprites na captura salva e registrar os
   limites da interpretação de shape `0`, shape `1` e shape `2`.
2. Continuar a validação de VDP2 por quadro: camada ativa, endereços lidos,
   prioridade/CRAM e comparação independente dos pixels compostos.
3. Só ao introduzir um backend GPU, adicionar dirty tracking testado para
   acessos que cruzam páginas e publicar VRAM/CRAM após os pontos corretos de
   sincronização.
4. Tratar BRAM persistente, segundo SH-2 real, BIOS mais ampla, CD-DA e SCSP
   completo como tarefas separadas, cada uma guiada por comportamento do
   corpus e testes diferenciais.
