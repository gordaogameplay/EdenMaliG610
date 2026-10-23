# DOC_TLS_INVESTIGACAO.md

Status: documento de analise da sessao de 23/10/2026. Nao comitado: os
ficheiros desta sessao ficam por revisar antes de virar topico (nao enviar
nada ao github hoje, a pedido).

Fontes usadas — todas medidas, nenhuma citada de memoria:

- `src/native_engine.cpp` 149-220 (a maquina do TLS), 745-820 (o `run()` e a
  troca de TLS), 1062-1180 (`run_guest_thread` e o trampolim de sinais),
  1540-1620 (o caso SIGSEGV e `emulate_tls_read`);
- logs LOG36/LOGI/LOGK/LOGL da board (run longo do Pokemon Shield);
- dump do `sdk.text` POS-patcher (base 0x8378d000), desassembled com
  capstone: 52 sites de `mrs tpidr_el0`, 0 de `tpidrro` (ver 2.4);
- o desassemble da wrapper 0x83ae9c10 e dos stubs do patcher
  0x83b0bb30..0x83b0bb94.

---

## 0. Correccao de leitura do LOG36 (prioritaria)

O ciclo anterior registou (PROGRESSO.md, 23/10, nota do fast path):

> o teste `cmp x9,x1` nunca acerta, 1135/1135 pelo ramo lento — aberto:
> design do guest ou o nosso tpidr esta' errado

**Esta afirmacao esta' errada e este documento corrige-a.** A prova e' o
proprio valor `x30` que os prints `[svc]` mostraram 1135 vezes:

```
0x83ae9c18  mrs  x9, tpidr_el0
0x83ae9c24  cmp  x9, x1
0x83ae9c28  b.eq #0x83ae9c3c        ; ramo RAPIDO
0x83ae9c2c  mov  x0, x1
0x83ae9c30  mov  x1, x8
0x83ae9c34  bl   #0x83b0bb6c        ; ramo LENTO  -> x30 = 0x83ae9c38
0x83ae9c38  b    #0x83ae9c44
0x83ae9c3c  mov  x0, x2
0x83ae9c40  bl   #0x83b0bb64        ; ramo RAPIDO -> x30 = 0x83ae9c44
0x83ae9c44  and  w9, w0, #0x3fffff
```

- `x30 = 0x83ae9c44` e' o return-site do `bl` que so' existe no ramo rapido
  (instrucao em 0x83ae9c40).
- O ramo lento deixaria `x30 = 0x83ae9c38` (return-site de 0x83ae9c34).
- Os 1135 prints mostraram x30 = 0x83ae9c44 sem excecao.

Conclusao corrigida: **o `cmp x9, x1` acerta em 1135 de 1135** — o guest
sempre entra no ramo rapido. Nao ha' desalinhamento detetado na wrapper;
a questao aberta passa a ser a qualidade do CONTEUDO do bloco de TLS, nao
o valor da base (ver secao 5).

Os stubs confirmam o mecanismo (secao 2.3): o site 0x83b0bb64 e' `brk
#0x21` e o 0x83b0bb6c e' `brk #0x22` — o imm do brk preserva o id do svc
original. O LR que o print ve' nao mente em nenhum dos dois.

---

## 1. A maquina TLS do motor hoje (mapa das linhas)

Quatro pecas, por ordem de execucao:

1. **O endereco do bloco vem do Eden.** `NativeEngine::run_guest_thread`
   (linha 1066) recebe `tls` e grava em `st->guest_tls` (linha 1083). O
   Eden aloca uma `ThreadLocalRegion` de `0x200` bytes por thread a partir
   das `KThreadLocalPage` (4 slots de 0x200 por pagina de 4 KiB —
   `vendor/eden/.../k_thread_local_page.h:29`) e zera o bloco
   (`k_thread.cpp:227-228`). O valor 0x88000000 que os logs mostram e' um
   slot desses: foi dele que a thread principal do guest ficou.
2. **O patcher reescreve o guest.** Ver o comentario de 149-156: o guest
   pede o TLS com `mrs x, tpidrro_el0`; neste host esse registo le sempre
   0 (medido quatro vias) e `msr tpidrro_el0, xn` levanta SIGILL
   (`arch_prctl(ARCH_SET_FS)` devolve -1 — medido, comentario 1578-1581).
   O patcher troca a instrucao por `mrs x, tpidr_el0`.
3. **O motor programa `TPIDR_EL0` por thread de host.** No `run()` (757-761)
   e no trampolim (1172-1175): antes do guest correr,
   `write_thread_pointer(st->guest_tls)`; antes do handler correr,
   `write_thread_pointer(st->host_tls)`. O registo e' fisicamente o `fs`
   da libc — por isso o handler descobre QUE thread faultou lendo o
   proprio registo na entrada (1150-1160), antes de trocar.
4. **A emulacao por SIGSEGV (fallback).** `emulate_tls_read` (200-260): o
   `mrs tpidrro` nao faulta (le 0); o fault chega na instrucao seguinte
   (`ldr x8, [x8, #0x1f8]`). O handler recua ate' 8 bytes de pc, procura o
   encoding `0xD53BD060|Rt` sob a mascara `0xFFFFFFE0` e emula a leitura
   inteira: injeta o TLS base no registo destino e salta a instrucao
   faultante.

---

## 2. A wrapper 0x83ae9c10 desassembled (dump POS-patcher)

```
0x83ae9c10  stp  x29, x30, [sp, #-0x10]!
0x83ae9c14  mov  x29, sp
0x83ae9c18  mrs  x9, tpidr_el0        ; a base do TLS DESTA thread (nativo)
0x83ae9c1c  mov  x8, x2
0x83ae9c20  and  x2, x0, #0xffffffff  ; handle = low32 do x0 recebido
0x83ae9c24  cmp  x9, x1               ; identidade: TLS vivo == x1
0x83ae9c28  b.eq #0x83ae9c3c
; ramo lento (re-servico):
0x83ae9c2c  mov  x0, x1
0x83ae9c30  mov  x1, x8
0x83ae9c34  bl   #0x83b0bb6c   ; brk #0x22 (WithUserBuffer)
0x83ae9c38  b    #0x83ae9c44
; ramo rapido (ja' servido):
0x83ae9c3c  mov  x0, x2
0x83ae9c40  bl   #0x83b0bb64   ; brk #0x21 (SendSyncRequest)
0x83ae9c44  and  w9, w0, #0x3fffff   ; sanitizacao do rc
0x83ae9c48  mov  w10, #0xf601
0x83ae9c4c  and  x8, x0, #0xffffffff
0x83ae9c50  cmp  w9, w10
0x83ae9c54  mov  w9, #0x5a0b
0x83ae9c58  movk w9, #2, lsl #16
0x83ae9c5c  csel x8, x9, x8, eq
```

Semantica (hipotese marcada; e' CANDIDATO a' confirmacao):

- O x1 recebido e' o **cookie de identidade do dono da sessao**. x9 e' a
  leitura VIVA do `TPIDR_EL0` desta thread. O teste pergunta "a sessao que
  vou usar ainda esta' ligada a' ESTA thread?"; se o cookie for o desta
  thread, salta o re-servico e envia pelo handle no low32 do x0.
- **Medido**: o x1 entra = 0x88000000 — o TLS base da thread principal.
  O cmp acerta, o ramo rapido corre, 1135/1135.
- Tres candidatos para a origem do x1 (sem confirmacao):
  (a) o proprio TLS base, lido pelo chamador logo antes de chamar a
  wrapper (`mrs x1, tpidr_el0` na rotina de cima) — por isso sempre acerta;
  (b) o valor guardado na estrutura da sessao no dia do bind;
  (c) um campo do bloco TLS que o guest repassa (os slots 0x1F0/0x1F8 da
  secao 3 sao candidatos naturais).

### 2.3 Os stubs de trap do patcher (a prova do x30)

```
0x83b0bb34  brk #0x1b    0x83b0bb3c  brk #0x1c    0x83b0bb44  brk #0x1d
0x83b0bb50  brk #0x1f    (stub com str x0,[sp,#-0x10]! e str w1,[x2] no retorno)
0x83b0bb64  brk #0x21    ; SendSyncRequest               0x83b0bb68  ret
0x83b0bb6c  brk #0x22    ; SendSyncRequestWithUserBuffer 0x83b0bb70  ret
0x83b0bb78  brk #0x23    (stub com sp de 16 bytes)
```

Factos fechados: cada site do patcher e' UM `brk #imm` so', com o imm = o
id do svc original (0x21 = 33 = SendSyncRequest, o mesmo id de
`include/g610/svc_dispatcher.hpp`). No Linux arm64 o `brk` chega como
SIGTRAP (nao SIGILL) — o motor recebe nos dois casos; e' o par
(pc do stub, 4 bytes) que identifica o site.

### 2.4 O dump e' POS-patcher (prova)

O comentario do motor (linhas 212-216) registou no offset 0x32f900 do sdk
a sequencia com `mrs x8, tpidrro_el0`. O dump atual no endereco
0x83abc900 (= base + 0x32f900) mostra o MESMO codigo com tpIDR:

```
0x83abc900  mrs  x8, tpidr_el0
0x83abc904  ldr  w9, [x0]           ; o spill medido no comentario
0x83abc908  ldr  x8, [x8, #0x1f8]
```

52 sites com tpidr, ZERO com tpidrro: o patcher cobriu todos os sites
visiveis deste modulo.

---

## 3. O que o guest toca no bloco TLS (scan das 52 sites)

Metodo: para cada `mrs Xt, tpidr_el0` (encoding `0xD53BD040|Rt`), olhar as
primeiras 6 instrucoes seguintes e colher os imediatos de `ldr/str` cuja
BASE e' o proprio Xt (acesso direto ao bloco); separar depois o que veio
por cadeia (`[tls+0x1f8] -> objeto -> +0x70`).

**Acesso direto (a primeira instrucao apos o mrs):**

| offset | sites | leitura |
|--------|-------|---------|
| 0x1F8 | 32 | O slot mais tocado do SDK: o guest GRAVA o ponteiro de TLS no proprio bloco (`str x0,[x1,#0x1f8]` — comentario da linha 35) e rele em todos os outros. E' o "thread corrente" do scheduler interno do nn::os. |
| 0x1F0 | 4 | Segundo slot perto do fim. Duas delas, 0x83c98920 e 0x83c98930, sao stubs de PLT com `mrs; ldr/str; ret` — o par de acessores get/set que o SDK expoe. |
| 0x000 | 1 | Na entrada de thread do CreateThread do SDK (0x83abb24c, logo depois do mrs): `str x0, [x1]`. |

**Por cadeia (via o objeto carregado de 0x1F8):** +0x70, +0x78 e +0x80 —
sao campos do objeto "thread corrente" apontado por [tls+0x1f8], nao do
bloco TLS em si.

Confronto com o spec de fora (CAUTELA: nao verificado contra os wikis nesta
sessao; e' o que o Eden e as 0x200 do spec descrevem):

- O kernel do Eden emula em cima do bloco os campos `interrupt_flag`
  (u16) e `disable_count` (u16) nos primeiros bytes
  (`k_thread.cpp:568-578`) e grava `thread_cpu_time` tambem dentro
  (`k_thread.cpp:601`). O guest medido faz accessos no 0x0 (1x) e nos
  dois slots do fim — nenhum conflito com o Eden hoje.
- O spec documenta o TLS de 0x200 bytes com os primeiros 0x10 zerados pelo
  kernel e o fim reservado ao SDK do nintendo. Os offsets que o guest usa
  COM CERTEZA sao os do fim (0x1F0/0x1F8) — e' o SDK quem os preenche no
  seu boot, nao o kernel.

**Conclusao da secao:** o layout que o motor precisa garantir hoje:

1. um bloco RW de 0x200 bytes no espaco do guest;
2. o endereco live no `TPIDR_EL0` da thread de host enquanto o guest corre;
3. o CONTEUDO e' trabalho do proprio SDK do guest — medido: o guest
   grava 0x1F0/0x1F8 por si. O motor nao precisa de preencher.

---

## 4. Onde pode quebrar amanha (analise de risco, nao medida)

- **Threads de guest a enviar para sessao de outra.** O x1 e' cookie de
  dono. No run atual 1135/1135 acertaram = as duas sessoes (0x981f1 e
  0xa01f0) foram servidas por quem as criou. Se outra thread de guest
  (ex.: a 0x28004, a suspeita do spin) comecar a enviar para uma delas, o
  cmp faltara' e o ramo lento (`brk #0x22`) entrara'. Se isso aparecer nos
  logs, e' o sinal de mao-dupla a verificar.
- **O init do conteudo.** O motor nao preenche 0x1F0/0x1F8. O Eden zera o
  bloco na criacao e o SDK preenche o que lhe interessa (medido). Hoje
  nao e' preciso init. A pergunta que fica: o run MUITO longo (60+ min)
  mantem isso? Nao verificado.
- **O encadeamento 0x1F8 -> objeto -> +0x70/+0x78** e' sensivel a qualquer
  re-uso do objeto pelo guest. Nada a fazer do lado do motor alem de
  garantir RW.

---

## 5. Tres abordagens C++ para amanha (`src/native_engine.cpp`)

As tres respondem a mesma pergunta: "o que fazer no motor para o bloco de
TLS do guest estar certo". Diferem no custo e no risco.

### A. Corrigir e partir do init (conservadora)

No reset do contexto de thread (onde o motor recebe `tls` do Eden),
escrever duas palavras a mais na area do Eden:

```cpp
// Pre-init do bloco do guest. O SDK do jogo ja' grava 0x1F8 e 0x1F0
// (medido); isto e' so' garantia de que os slots nao comecam com lixo
// do pool de paginas do Eden.
*g610::guest_word(tls + 0x1F0) = tls;
*g610::guest_word(tls + 0x1F8) = tls;
```

- Pro: dez linhas, um teste unitario (as duas escritas e o encode `str`).
- Contra: escreve por cima de algo que a thread vai escrever; potencial
  "dono" falsificado se o guest ler o slot antes do proprio `str`
  (nao medido).
- Papel: fecha a pergunta do init de uma vez, sem tocar em sinais.

### B. Mapa proprio de TLS por thread (media de custo, alto risco)

Em vez de confiar no `tls` que o Eden entrega, o motor pode alocar os
blocos num pool proprio (mmap de gamas fixas dentro do espaco de guest
reservado — o mesmo padrao do watchpoint) e programar o `TPIDR_EL0` com o
endereco do pool. O patcher e a troca ficam iguais; so' muda o ENDERECO
que o `write_thread_pointer` usa e a chave da tabela de estado.

- Pro: controle total do conteudo (o motor zera com o layout da secao 3
  ao criar cada thread de guest).
- Contra: o guest guarda referencias ao endereco ANTERIOR em objetos
  (o "thread corrente" apontado por [tls+0x1F8] referencia o ThreadType,
  que o guest criou dentro de 0x88000000). Teria de preencher a iniciacao
  com o ThreadType real, e o motor nao sabe onde ele esta'. Alto risco de
  cascata sobre algo que HOJE funciona.

### C. Deixar de ter handler para o caso (eliminacao do caminho)

A wrapper e as 52 sites de `mrs tpidr` JA correm nativamente: o patcher
trocou tpidrro->tpidr e o motor programa o registador por thread. NAO
existe trap na leitura do TLS hoje. O caminho SIGSEGV + `emulate_tls_read`
serve QUANDO uma passada do patcher nao cobriu um site. Fechar isso:

```cpp
// No 2o passe do patcher: verificar que nao sobra nenhum site
// 0xD53BD060|Rt (o encoding ORIGINAL do tpidrro) sem reescrita, nem
// nenhum acesso [x,#0x1f8] em caminho novo (ex.: modulo 2).
```

- Pro: se o passe cobrir tudo, o fallback SIGSEGV MORRE para o caso de
  TLS — menos codigo no handler de sinal (o objetivo do tema fast-path).
- Contra: perde-se um mecanismo de prova (a emulacao hoje e' tambem
  telemetria de onde o guest le o TLS).

**Recomendacao entre as tres:** primeiro A (barata e legivel), depois C
com medida (o run longo limpo e' a prova), B nao por agora — 0x88000000
e' o endereco que o Eden da' hoje; o caminho menos perturbado e' manter
o endereco e solidificar o conteudo.

---

## 6. Medidas citadas (tabela)

| medida | fonte | valor |
|--------|-------|-------|
| envios pela wrapper | LOGK, grep `[svc]` das duas irma (recontagem desta sessao) | 1135/1135 com x30=0x83ae9c44 — o ramo rapido em TODAS; o ramo lento deixaria x30=0x83ae9c38 |
| handles das sessoes | LOGL prints `[svc #...]` | 0x981f1 e 0xa01f0 |
| x1 no ponto do brk | LOG36 | 0x88000000 (estavel no run) |
| TLS base da principal do guest | idem (o cmp x9,x1 acerta com x9 vivo) | 0x88000000 |
| sites de mrs-tls no dump | scan local (capstone) | 52 tpidr, 0 tpidrro |
| acesso direto na base | scan local (primeira instr apos o mrs) | +0x1F8 (32x), +0x1F0 (4x), +0x000 (1x) |
| cadencia com | LOGK `[ipc ]` | 0x981f1 cmd 0x10 @ 58 Hz (866 envios em ~15 s), 0xa01f0 cmd 0xc0b @ 18 Hz (264 envios) |
| contas do `[ipc ] #33` | LOGK | 1319 no run: 866 + 264 = as duas irma; ~184 de outras sessoes |
| corpos de msg | LOGK `[ipc ] #33` | 981f1: [01000006][0010][token var][0xd9b9xxxx][1][0]; a01f0: [01110006][c0b][tag {283000|282000|202000|82000}][0fffe5xx][0][0]; so' a chamada de setup difere (os pares {0,0xE}/{6,12} do diario = os W4/W5 dela, nao resposta) |
| stamps CMIF | LOGK `[ipc ] #33` das outras sessoes | W4 = `SFCI` (0x49434653) nas requests de 0x101fe/0x181fd... e `SFCO` (0x4f434653) no mesmo lugar; nas duas irma nada nos W0..W5 (stamp alem da janela de 24 bytes ou outra familia) — Switchbrew "IPC Marshalling" (nn::sf::cmif) |

Pecas ainda por verificar (marcadas no texto): a origem exata do x1
(secao 2), o spec de fora com link do wiki (secao 3), o comportamento do
run longo (secao 4).
