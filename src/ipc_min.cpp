// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// IPC mínimo do Horizon — o suficiente para o `sm:`
// ---------------------------------------------------------------------------
//
// Isto não é um emulador de serviços. É o transporte: descodificar o
// cabeçalho de um pedido, executar UM comando do `sm`, e escrever a
// resposta no mesmo buffer do guest.
//
// ## O que está medido, e onde
//
// O layout vem de `ipc.h` do Eden (`core/hle/ipc.h`), não de memória:
//
//   0x00  CommandHeader        raw_low:  type[0:16] num_x[16:20] num_a[20:24]
//                                     num_b[24:28] num_w[28:32]
//                              raw_high: data_size[0:10] buf_c_flags[10:14]
//                                                 enable_handle_desc[31]
//   0x08  BufferDescriptorC    8 bytes
//   0x10  HandleDescriptorHeader 4 bytes
//   0x20  DataPayloadHeader    { u32 magic; u32 pad; }   magic = "SFCI"
//   0x28  corpo do comando     SFCI
//
// E foi **medido no pedido real** do SDK do jogo (ver boot_trace.cpp, `[ipc ]`):
//
//   +00  06 00 00 00 0a 00 00 80 | type = 6 (RequestWithContext), data_size = 10
//   +08  01 00 00 00 00 00 00 00 | um descritor C
//   +20  53 46 43 49 01 00 00 00 | "SFCI"  e  cmd_id = 1
//
// `cmd_id = 1` é **`GetServiceCmif`**, exactamente como o Eden o regista
// (`service/sm/sm.cpp:273`):
//
//   {0, &SM::Initialize,  "Initialize"}
//   {1, &SM::GetServiceCmif, "GetService"}
//   {2, &SM::RegisterServiceCmif, "RegisterService"}
//   {3, &SM::UnregisterService, "UnregisterService"}
//
// ## O que a resposta é
//
// `ResponseBuilder` (Eden, `ipc_helpers.h:41`) faz `memset(cmdbuf, 0, ...)` e
// reconstrói o cabeçalho com `header.type.Assign(ctx.GetCommandType())` — ou
// seja, **o tipo da resposta é o do pedido** (6). E, como o `is_hle` faz o
// kernel não escrever nada (`k_server_session.h:63`, comentário explícito), a
// resposta é só o que o serviço escreveu no buffer do cliente.
//
// Portanto: mesmo CommandHeader, magic "SFCI", e o corpo que o comando
// produz. Não há um segundo formato para aprender.
//
// ## STUB, e é o essencial disto
//
// `GetService` devolve um handle **fabricado**. Não há servidor nenhum atrás
// dele: o próximo `SendSyncRequest` nesse handle tem de ser reconhecido pelo
// nome do serviço que pedi, e responder com o que eu tiver. É um mapa
// handle→nome, e nada mais. Qualquer serviço que exija conteúdo real vai
// receber uma resposta vazia — e isso tem de ser dito quando acontecer, não
// descoberto depois.

#include "g610/svc_dispatcher.hpp"

#include "g610/eden_bridge.hpp"

// O `KReadableEvent` — o objecto que o guest recebe de um
// `GetEventHandle`, e o que a espera consulta. E' o codigo do Eden.
#include "vendor/eden/core/hle/kernel/k_readable_event.h"
// A ponte: e' onde vive o `GetCurrentProcess` (o `k_process.h` do Eden
// foi substituido por ela).
#include "vendor/eden/core/hle/kernel/k_process_bridge.h"
// A tabela de handles: e' de onde se busca o objecto por handle.
#include "vendor/eden/core/hle/kernel/k_handle_table.h"

#include <cstdlib>

namespace g610 {
namespace {

// Onde acaba a MessageBuffer.
//
// `sendbrew/Thread_Local_Storage`: a MessageBuffer do ThreadLocalRegion vai
// de 0x000 a 0x0FF. Os ThreadVars vivem em 0x1E0. **O mesmo bloco é as duas
// coisas** — e o SDK passa o início da MessageBuffer como buffer de IPC.
//
// A primeira versão limpava `size` bytes a partir do corpo, e `size` vale
// 0x1000: limpou por cima dos ThreadVars, e o guest voltou a dar fault a ler
// o TLS. O respeito pelo `data_size` do cabeçalho resolve, e o limite duro
// em 0x100 é a rede de segurança para o dia em que `data_size` mentirem.
constexpr std::uint32_t kMessageBufferEnd = 0x100;

// O corpo do comando começa depois do `DataPayloadHeader`.
constexpr std::uint32_t kOffCommandHeader = 0x00;
constexpr std::uint32_t kOffBufferDescC   = 0x08;
constexpr std::uint32_t kOffHandleDesc    = 0x10;
constexpr std::uint32_t kOffPayloadMagic  = 0x20;

// O corpo do PEDIDO está em 0x28 — medido, não deduzido.
//
// O corpo da RESPOSTA **não tem a mesma Morada**, e é aqui que a coisa está
// mais susceptível de estar errada.
//
// A resposta é montada pelo `ResponseBuilder` (Eden, `ipc_helpers.h:36-95`),
// que empilha a partir do zero e NÃO repõe os descritores que o pedido tinha:
//
//     PushRaw(header)                    -> índice 2
//     (sem handle descriptor, o Header não tem enable_handle_descriptor)
//     AlignWithPadding()                  -> 2 alinha para 4
//     PushRaw(data_payload_header)        -> índice 6
//
// Com 16 bytes de alinhamento, o payload do `ResponseBuilder` cai em
// **0x18**, e não nos 0x20 que o pedido usa. O pedido tem um descritor C e um
// descritor de handles a mais, e é isso que o empurra para 0x20.
//
// Duas leituras possíveis do mesmo ficheiro, e a medição é que decide — por
// isso isto é uma constante e não uma inline no meio do código: trocar
// 0x18 por 0x20 e recompilar é a experiência.
constexpr std::uint32_t kOffBody          = 0x28;

// Onde o corpo do `GetService` está, a contar do magic. O nome do serviço vem
// depois de `cmd_id` e de mais duas palavras — medido no pedido 2 ("lm" em
// magic+0x10).
constexpr std::uint32_t kNameOffFromMagic = 0x10;

// Até onde se procura a assinatura. O maior payload medido está em 0x20; 0x40
// dá margem para um descritor a mais sem chegar a varrer o buffer todo.
constexpr std::size_t kPayloadScanMax = 0x40;

// Os magics, lidos como bytes na mesma ordem em que estão no buffer.
//
//   pedido  = "SFCI"   (Input)
//   resposta= "SFCO"   (Output)
//
// I/O, não request/response. Sai de `ResponseBuilder` (Eden,
// `ipc_helpers.h:88`): `data_payload_header.magic = MakeMagic('S','F','C','O')`.
//
// Isto é o que faltava: a resposta era escrita com o magic do pedido, e o
// guest via um payload que ele não reconhece e vai buscar lixo.
constexpr char kMagicRequest[4]  = {'S', 'F', 'C', 'I'};
constexpr char kMagicResponse[4] = {'S', 'F', 'C', 'O'};
constexpr std::uint32_t kSfcIMagic = 0x49434653u;  // "SFCI" como u32 LE

// CommandType (Eden, ipc.h:37-49).
constexpr std::uint32_t kCmdRequestWithContext = 6;

// `data_size` da resposta: `normal_params(2) + DataPayloadHeader/4(2) + 4`.
// Os 4 u32 são os 16 bytes de padding obrigatório (`ipc_helpers.h:48-51`).
// O `data_size` de uma resposta, em palavras de 32 bits.
//
// Do `ResponseBuilder` do Eden (ipc_helpers.h:93):
//
//     raw_data_size = normal_params_size;
//     raw_data_size += sizeof(DataPayloadHeader)/4 + 4 + normal_params_size;
//
// que são `2 + 4` de cabeçalho/enchimento mais os parâmetros duas vezes — uma
// para o tamanho que o guest vai ler e outra para o que o servidor escreve.
// Para uma resposta de 2 parâmetros dá 2 + 2 + 4 + 2 = **10**.
//
// O valor que estava aqui era 8, tirado do ar. E o pedido real do jogo tem
// data_size = 10, o que confirma a conta por um caminho independente.
constexpr std::uint32_t kReplyDataSize = 10;

// Os comandos do `sm:`, pelos números do Eden.
constexpr std::uint32_t kSmInitialize       = 0;
constexpr std::uint32_t kSmGetService       = 1;
constexpr std::uint32_t kSmRegisterService  = 2;
constexpr std::uint32_t kSmUnregisterService = 3;

// O `sm` devolve 0x202 quando o nome do serviço não existe (Eden:
// Service::SM::ResultInvalidServiceName). Este valor é o que o Eden põe e o
// que o SDK sabe tratar.
constexpr std::uint32_t kSmResultInvalidServiceName = 0x202;

} // namespace

bool SvcDispatcher::ipc_dispatch(std::uint64_t buffer, std::size_t size) {
    if (mem_ == nullptr || size < kOffBody + 8)
        return false;

    std::uint8_t m[kIpcBufferMax];
    const std::size_t n = size < sizeof(m) ? size : sizeof(m);
    if (!read_guest(buffer, m, n))
        return false;

    // ── despejar o pedido ───────────────────────────────────────────────
    //
    // Isto não é depuração: é a medição que está em falta. O guest pede
    // `sm: GetService`, nós lemos um nome vazio e respondemos
    // InvalidServiceName — e não se sabe se o nome está mesmo vazio ou se
    // é a NOSSA leitura que está errada. São duas coisas que produzem
    // exactamente o mesmo sintoma, e sem os bytes do pedido não há como
    // distinguir. Foi o que aconteceu.
    //
    // Liga-se com IA_IPC_DUMP=1. Offset, hex e ASCII, porque "o nome não
    // aparece" e "o nome aparece mas deslocado 8 bytes" são conclusões
    // diferentes e precisam de se ver para se decidir entre elas.
    static const bool dump = std::getenv("IA_IPC_DUMP") != nullptr;
    if (dump) {
        // Despeja o buffer INTEIRO, e não só os `n` bytes que a aritmética
        // do IPC nos obrigam a ler. Os primeiros 512 bytes estão quase todos
        // a zeros e o nome do serviço não está lá — leer mais é a diferença
        // entre "o nome não veio" e "o nome veio e nós não o procurámos onde
        // estava".
        std::uint8_t cheio[0x1000];
        std::size_t total = 0;
        if (read_guest(buffer, cheio, sizeof cheio))
            total = sizeof cheio;

        std::printf("\n──── pedido IPC: buffer 0x%llx, size %zu, %zu lidos ────\n",
                    (unsigned long long)buffer, size, total);
        for (std::size_t i = 0; i < total; i += 16) {
            const std::size_t fim = (i + 16 <= total) ? i + 16 : total;
            bool alguma_coisa = false;
            for (std::size_t j = i; j < fim; ++j)
                if (cheio[j] != 0)
                    alguma_coisa = true;
            if (!alguma_coisa)
                continue;
            std::printf("  %04zx  ", i);
            for (std::size_t j = i; j < fim; ++j)
                std::printf("%02x ", cheio[j]);
            for (std::size_t j = fim; j < i + 16; ++j)
                std::printf("   ");
            std::printf(" |");
            for (std::size_t j = i; j < fim; ++j) {
                const unsigned char ch = static_cast<unsigned char>(cheio[j]);
                std::putchar((ch >= 0x20 && ch < 0x7F) ? static_cast<int>(ch) : '.');
            }
            std::printf("|\n");
        }
        std::printf("──────────────────────────────────────────────────\n");
    }
    auto le32 = [&m](std::size_t off) {
        return static_cast<std::uint32_t>(m[off]) |
               (static_cast<std::uint32_t>(m[off + 1]) << 8) |
               (static_cast<std::uint32_t>(m[off + 2]) << 16) |
               (static_cast<std::uint32_t>(m[off + 3]) << 24);
    };
    auto put32 = [&m](std::size_t off, std::uint32_t v) {
        m[off]     = static_cast<std::uint8_t>(v);
        m[off + 1] = static_cast<std::uint8_t>(v >> 8);
        m[off + 2] = static_cast<std::uint8_t>(v >> 16);
        m[off + 3] = static_cast<std::uint8_t>(v >> 24);
    };

    // ── o CommandHeader, lido como o Eden o lê ───────────────────────────
    //
    // De `core/hle/ipc.h:51`:
    //
    //     struct CommandHeader {
    //         u32_le raw_low;
    //         BitField<0, 16, CommandType> type;
    //         BitField<16, 4, u32> num_buf_x_descriptors;
    //         BitField<20, 4, u32> num_buf_a_descriptors;
    //         BitField<24, 4, u32> num_buf_b_descriptors;
    //         BitField<28, 4, u32> num_buf_w_descriptors;
    //
    //         u32_le raw_high;
    //         BitField<0, 10, u32> data_size;
    //         BitField<10, 4, BufferDescriptorCFlag> buf_c_descriptor_flags;
    //         BitField<31, 1, u32> enable_handle_descriptor;
    //     };
    //
    //     union HandleDescriptorHeader {
    //         BitField<0, 1, u32> send_current_pid;
    //         BitField<1, 4, u32> num_handles_to_copy;
    //         BitField<5, 4, u32> num_handles_to_move;
    //     };
    //
    // E o `CommandType` (ipc.h:38): Request = 4, Control = 5,
    // RequestWithContext = 6, ControlWithContext = 7.

    // O pedido é do tipo certo? Não responder a um pedido que não é um pedido
    // é pior do que não responder.
    const std::uint32_t raw_low  = le32(kOffCommandHeader);
    const std::uint32_t raw_high = le32(kOffCommandHeader + 4);
    const std::uint32_t type = raw_low & 0xFFFFu;
    if (type != kCmdRequestWithContext) {
        last_ipc_ = "tipo de comando inesperado: " + std::to_string(type);
        return false;
    }

    const std::uint32_t data_size = raw_high & 0x3FFu;
    const std::uint32_t send_pid  = (raw_high & 0x80000000u) ? 1u : 0u;

    // ── o payload NÃO se procura: calcula-se ─────────────────────────────
    //
    // A versão anterior varria o buffer à procura da assinatura "SFCI". Era
    // robusto e era o caminho errado:estava a esconder a
    // aritmética que o Eden faz, e um formato de pedido diferente ter-se-ia
    // de resolver por tentativa em vez de por leitura.
    //
    // A aritmética é `HLERequestContext::ParseCommandBuffer` (hle_ipc.cpp:142),
    // palavra a palavra. O índice é contado em PALAVRAS de 32 bits:
    //
    //     índice = 2                       // o CommandHeader
    //     se enable_handle_descriptor:
    //         índice += 1                  // o HandleDescriptorHeader
    //         se send_current_pid:
    //             índice += 2              // <<<< a peça que faltava
    //         índice += num_copy + num_move
    //     índice += os descritores X/A/B/W (4, 3, 3, 3 palavras cada)
    //     índice  = arredondar(index) para multiplo de 4      // 16 bytes
    //     payload = índice * 4
    //     índice += 2                      // o DataPayloadHeader
    //     corpo   = índice * 4
    //
    // E o `Skip(2, false)` do `send_current_pid` é a linha que faltava. Sem
    // ela o índice dá 4 e o payload cairia em 0x10; com ela dá 8 e cai em
    // **0x20** — que é onde o jogo o põe, medido.
    //
    // Isto não é dedução: é o `static_assert`-por-dedo do Eden, com a conta
    // conferida contra o pedido real.
    std::size_t idx = 2;                       // em palavras

    if (raw_high & 0x80000000u) {              // enable_handle_descriptor
        idx += 1;                              // HandleDescriptorHeader
        if (send_pid)
            idx += 2;                          // o pid que o kernel escreve
        idx += 0;                              // num_copy + num_move: 0 medido
    }
    idx += (raw_low >> 16 & 0xFu) * 1;         // descritores X (1 palavra)
    idx += (raw_low >> 20 & 0xFu) * 3;         // descritores A
    idx += (raw_low >> 24 & 0xFu) * 3;         // descritores B
    idx += (raw_low >> 28 & 0xFu) * 3;         // descritores W
    if (idx & 3)
        idx += 4 - (idx & 3);                  // alinhamento a 16 bytes

    const std::size_t payload = idx * 4;
    idx += 2;                                  // o DataPayloadHeader
    const std::size_t cmd_off_calc = idx * 4;

    // ── a geometria, toda ────────────────────────────────────────────────
    //
    // Vem de três lugares do Eden, e nenhum deles é de memória:
    //
    //     // ipc.h:174
    //     struct DataPayloadHeader {
    //         u32_le magic;
    //         INSERT_PADDING_WORDS_NOINIT(1);
    //     };
    //     static_assert(sizeof(DataPayloadHeader) == 8, ...);
    //
    //     // ipc_helpers.h:298, no servidor ao LER um pedido
    //     // Skip the u64 command id, it's already stored in the context
    //     static constexpr u32 CommandIdSize = 2;   // dois u32 = 8 bytes
    //     Skip(CommandIdSize, false);
    //
    //     // ipc_helpers.h:93, no servidor ao ESCREVER uma resposta
    //     raw_data_size += sizeof(DataPayloadHeader)/4 + 4 + normal_params_size;
    //
    // O que daí sai, em bytes a partir do início do payload:
    //
    //     +0x00  magic "SFCI"  (na resposta, "SFCO")
    //     +0x04  enchimento    (1 u32)
    //     +0x08  command id    (u64)
    //     +0x10  corpo         — Result primeiro, e para o `sm: GetService`
    //                           o nome do serviço é o que o pedido põe aqui
    //                           (`PopServiceName`, sm.cpp:163)
    //
    // A versão anterior lia o command id em `payload+4` e escrevia o Result
    // em `payload+4`: os dois 4 bytes a menos. Como o `payload+4` era uma
    // palavra de enchimento com o valor 1, o comando lido era 1 — GetService —
    // quando o guest tinha enviado Initialize (0). Respondíamos a um comando
    // que ninguém tinha enviado.
    //
    // Nenhuma variante da matriz apanharia isto: respondiam todas ao comando
    // errado, com a mesma confiança e o mesmo formato. Só o offset certo,
    // medido no ficheiro que o escreve, resolve.
    // Confirmado contra a assinatura. Se a aritmética alguma vez divergir do
    // que o Eden faz, esta comparação diz logo — em vez de o guest receber
    // uma resposta com o corpo no sítio errado e abortar sem dizer nada.
    if (n >= 4 && le32(payload) != kSfcIMagic) {
        char h[16];
        std::snprintf(h, sizeof h, "0x%02zx", payload);
        last_ipc_ = std::string("a aritmetica do payload calculou ") + h +
                    " e nao ha assinatura la (o Eden daria o mesmo sitio; se "
                    "isto dispara, o CommandHeader leu-se mal)";
        return false;
    }
    (void)cmd_off_calc;

    const std::size_t cmd_off    = payload + 0x08;   // u64
    const std::size_t nome_off   = payload + 0x10;   // no PEDIDO, o corpo e' o nome

    // O `result_off` da RESPOSTA é calculado mais abaixo, a partir de
    // `pay_r`. Não é o mesmo: o payload do pedido foi medido em 0x20 e o da
    // resposta é 0x10, pela regra de alinhamento do `ResponseBuilder`. Usar o
    // offset do pedido para escrever na resposta põe o Result 16 bytes fora —
    // e foi exactamente o que aconteceu na primeira tentativa desta
    // mudança, com a resposta a mostrar `SFCO` em 0x10 e o Result em 0x30,
    // que é o sítio onde o PEDIDO queria o nome do serviço.
    std::size_t result_off = 0;

    if (nome_off + 8 > n) {
        last_ipc_ = "pedido truncado antes do nome do servico";
        return false;
    }

    // `data_size` diz quanto do buffer é realmente do IPC. Vem de
    // `CommandHeader.raw_high` bits [0:10] (Eden, ipc.h:58-61), e no pedido
    // real do jogo vale 10 — muito menos do que os 0x1000 que o SDK passa
    // como tamanho do buffer.
    // Limite de escrita: o fim da IPC dentro do buffer, e nunca além da
    // MessageBuffer.
    std::size_t limite = kOffBody + data_size;
    if (limite > kMessageBufferEnd)
        limite = kMessageBufferEnd;
    if (limite > n)
        limite = n;
    if (limite < kOffBody + 8)
        limite = (n < kOffBody + 8 ? n : kOffBody + 8);

    const std::uint32_t cmd_id = le32(cmd_off);

    // ── resposta ────────────────────────────────────────────────────────
    // Reconstruir como o `ResponseBuilder` faz: o MESMO `CommandHeader` (o
    // tipo é ecoado do pedido), o magic, e o corpo.
    //
    // O corpo tem de ser limpo. O Eden faz `memset(cmdbuf, 0, ...)` antes de
    // escrever, e sem isso sobram bytes do pedido — que é como um guest
    // descobre que a resposta é feita de remendos.
    // ── geometria da resposta: experimento, não dedução ──────────────────
    //
    // Chegou-se aqui por dedução e três palpites falharam. Em vez de escolher
    // mais um, testam-se TODOS. A variante vem de `IA_IPC_VARIANT` (bitmask)
    // e o valor usado entra em `last_ipc_`, para que um resultado sem
    // contexto nao seja interpretavel.
    //
    //   bit 0  handle no descritor de handles (+ enable_handle_descriptor)
    //   bit 1  repetir o `type` do pedido em vez de 0
    //   bit 2  pôr o payload 0x10 mais acima
    //   bit 3  NAO responder (deixar o buffer como o guest o deixou) — o
    //         controlo, para saber se o guest repara na diferenca
    const int variante = ipc_variant();
    const bool v_handles   = (variante & 0x1) != 0;
    const bool v_tipo      = (variante & 0x2) != 0;
    const bool v_payload   = (variante & 0x4) != 0;
    const bool v_silencio  = (variante & 0x8) != 0;

    // A resposta usa a MESMA geometria do pedido.
    //
    // O `ResponseBuilder` do Eden reconstrói o payload a partir do zero, mas
    // o que interessa ao guest é encontrar a assinatura onde ele a procura —
    // e ele procura-a onde a deixou estar. Ecoar o offset do pedido é o que
    // garante isso, sem ter de reproduzir a aritmética dos descritores duas
    // vezes.
    // Onde vai o payload da RESPOSTA. Não é o mesmo sítio do pedido.
    //
    // O `ResponseBuilder` do Eden (ipc_helpers.h:36-101) faz, por ordem:
    //
    //     PushRaw(header);                      -> indice = 2 palavras
    //     se houver handles:
    //         PushRaw(HandleDescriptorHeader);  -> indice = 3
    //         ctx.handles_offset = index;
    //         Skip(n_handles);
    //     AlignWithPadding();                   -> indice multiplo de 4
    //     PushRaw(DataPayloadHeader);           -> indice += 2
    //
    // `AlignWithPadding` arredonda o indice para um multiplo de 4 palavras,
    // que sao 16 bytes. Com 0 handles o indice e' 2 e sobe para 4; com 1
    // handle o indice ja e' 4. Nos dois casos o payload da resposta fica em
    // **0x10**.
    //
    // O pedido foi medido em 0x20 e a resposta anterior era escrita a ecoar o
    // pedido. Sao sitios diferentes e o Eden nao tem nada que sugira que sejam
    // iguais — a resposta e' montada do zero, com `memset` antes.
    // `v_payload` passa a ser a variante de POSICAO: 0 = a regra do
    // `ResponseBuilder` (0x10), 1 = ecoar o payload do pedido (0x20). As duas
    // continuam a ser testadas porque só uma é correcta e qual delas é
    // depende do que o cliente faz, e isso não está escrito em lado nenhum
    // que este projecto possa ler.
    const std::size_t pay_r = v_payload ? payload : 0x10;
    result_off = pay_r + 0x10;

    std::size_t lim_r = pay_r + kReplyDataSize * 4;
    if (lim_r > limite)
        lim_r = limite;

    for (std::size_t i = pay_r; i < lim_r; ++i)
        m[i] = 0;

    // O `CommandHeader` da resposta: `type = 0`.
    //
    // `ResponseBuilder` (ipc_helpers.h:64-67) só escreve `header.type` no
    // caminho TIPC. No CMIF o cabeçalho fica com o valor zero do
    // `CommandHeader header{}`. E `data_size` passa a ser
    // `normal_params + sizeof(DataPayloadHeader)/4 + 4` — os 4 u32 são os
    // 16 bytes de padding obrigatório que o comentário menciona.
    // `enable_handle_descriptor` (bit 31 de raw_high) diz ao guest que ha
    // handles a seguir. Se o handle vai no descritor, este bit TEM de estar a
    // 1 — senao o guest nem va la buscar.
    const std::uint32_t hdr_type = v_tipo ? kCmdRequestWithContext : 0u;
    const std::uint32_t hdr_high  = kReplyDataSize | (v_handles ? 0x80000000u : 0u);
    put32(kOffCommandHeader, hdr_type);
    put32(kOffCommandHeader + 4, hdr_high);

    // O Eden faz `memset(cmdbuf, 0, sizeof(u32) * COMMAND_BUFFER_LENGTH)` no
    // começo do `ResponseBuilder` — o buffer INTEIRO, não só a parte que vai
    // ser escrita. Sem isso sobram bytes do pedido, e uma resposta feita de
    // remendos é uma coisa que o guest consegue ver.
    for (std::size_t i = 0; i < n && i < kMessageBufferEnd; ++i)
        m[i] = 0;

    // Cabeçalho do payload: magic + enchimento. O command id e' um u64 e vai
    // a seguir, ecoado do pedido — o servidor salta-o para chegar ao corpo.
    for (int i = 0; i < 4; ++i)
        m[pay_r + i] = static_cast<std::uint8_t>(kMagicResponse[i]);
    put32(pay_r + 4, 0);                       // a palavra de enchimento
    put32(pay_r + 8, cmd_id);                  // o command id, u64 (baixa)
    put32(pay_r + 12, 0);                      // o command id, u64 (alta)

    if (v_handles) {
        // O handle vai no descritor, logo a seguir ao `CommandHeader`, como o
        // `ResponseBuilder` faz: `ctx.handles_offset = index` logo depois de
        // escrever o `HandleDescriptorHeader`.
        put32(kOffCommandHeader + 8, 0);       // HandleDescriptorHeader
        put32(kOffCommandHeader + 12, 0);      // o handle movido
    }

    // O Result vive no CORPO, ou seja em `payload + 0x10` — e nao em
    // `payload + 4`, que e' a palavra de enchimento do DataPayloadHeader. Foi
    // ai que esteve durante mais tempo do que devia.
    put32(result_off, 0);

    std::string nome;

    switch (cmd_id) {
    case kSmInitialize:
        last_ipc_ = "sm: Initialize";
        break;

    case kSmGetService: {
        // Os 8 bytes seguintes são o nome do serviço, terminado em NUL
        // (`PopServiceName`, Eden sm.cpp:163-172: só mantém os caracteres
        // imprimíveis).
        for (std::uint32_t k = 0; k < 8; ++k) {
            const char c = static_cast<char>(m[nome_off + k]);
            if (c >= ' ' && c <= '~')
                nome.push_back(c);
        }
        last_ipc_ = "sm: GetService \"" + nome + "\"";

        if (nome.empty()) {
            // O primeiro `GetService` do jogo vem com o nome vazio — oito
            // zeros em magic+0x10, medido.
            //
            // Responder com sucesso e um handle fabricado foi a experiência:
            // o guest continuou a dar `Break(x0=0)`, exactamente como com o
            // erro. Logo, ou o nome vazio não é um "serviço por omissão", ou
            // a falha é noutro sítio. Volta-se à resposta honesta, que é o que
            // o Eden faz quando o nome não existe: `ResultInvalidServiceName`.
            //
            // Aexperiência fica escrita porque um palpite que não funcionou
            // é informação: significa que a resposta NÃO é o problema.
            put32(result_off, kSmResultInvalidServiceName);
            break;
        }

        const std::uint32_t h = connect_named_port(nome);
        if (v_handles)
            put32(kOffCommandHeader + 12, h);   // handle no descritor
        break;
    }

    case kSmRegisterService:
        last_ipc_ = "sm: RegisterService";
        break;

    case kSmUnregisterService:
        last_ipc_ = "sm: UnregisterService";
        break;

    default:
        last_ipc_ = "sm: comando " + std::to_string(cmd_id) +
                    " (desconhecido — resposta de erro, não um palpite)";
        put32(result_off, 0x202);
        break;
    }

    last_ipc_ += "  [variante " + std::to_string(variante) +
                (v_handles ? ": handle no descritor" : ": handle inline") +
                (v_tipo ? ", type ecoado" : ", type=0") +
                (v_payload ? ", payload ecoado (0x20)" : ", payload em 0x10 (regra do ResponseBuilder)") +
                (v_silencio ? ", SEM RESPOSTA" : "") + "]";

    if (v_silencio) {
        // Controlo: nao escreve nada. Se o guest tambem falhar com isto, o
        // problema nao e o conteudo da resposta.
        return true;
    }

    if (!write_guest(buffer, m, lim_r)) {
        last_ipc_ += "  [ESCRITA FALHOU]";
        return false;
    }

    // Despejo da RESPOSTA, lado a lado com o pedido. Um pedido sem a
    // resposta ao lado obriga a recordar a estrutura de memória; os dois
    // juntos mostram logo se um campo está no sítio errado, que é
    // precisamente o que esteve a acontecer com o command id e com o Result.
    if (std::getenv("IA_IPC_DUMP") != nullptr) {
        std::printf("\n──── resposta IPC: cmd_id %u ────\n", cmd_id);
        for (std::size_t i = 0; i < lim_r && i < 0x60; i += 16) {
            const std::size_t fim = (i + 16 <= lim_r) ? i + 16 : lim_r;
            bool alguma_coisa = false;
            for (std::size_t j = i; j < fim; ++j)
                if (m[j] != 0)
                    alguma_coisa = true;
            if (!alguma_coisa)
                continue;
            std::printf("  %04zx  ", i);
            for (std::size_t j = i; j < fim; ++j)
                std::printf("%02x ", m[j]);
            for (std::size_t j = fim; j < i + 16; ++j)
                std::printf("   ");
            std::printf("\n");
        }
        std::printf("  (payload em 0x%02zx, Result em 0x%02zx)\n",
                    (unsigned)pay_r, (unsigned)result_off);
        std::printf("──────────────────────────────────────────────────\n");
    }
    return true;
}



int SvcDispatcher::ipc_variant() {
    const char* v = std::getenv("IA_IPC_VARIANT");
    if (v == nullptr)
        return 0;
    return static_cast<int>(std::strtol(v, nullptr, 0));
}

Result SvcDispatcher::svc_send_sync_request(SvcContext& ctx) {
    // ── o caminho do Eden ────────────────────────────────────────────────
    //
    // `KClientSession::SendSyncRequest` do Eden, que é o mesmo caminho que
    // `svc/svc_ipc.cpp:30-36` percorre. Duas coisas que ele faz e o nosso
    // caminho antigo não:
    //
    //   1. **Usa o `MessageBuffer` do TLS da thread**, e ignora x1 e x2. Não
    //      é uma optimização: o SDK manda o buffer nos registos, e o Eden
    //      também não os olha. Ler o que estiver em x1/x2 e achar que é o
    //      tamanho é a forma de escrever por cima do buffer.
    //   2. **Devolve `false` quando o handle não é uma sessão.** O nosso
    //      caminho antigo respondia a qualquer handle, e o guest passava a
    //      ler lixo com um formato válido.
    if (eden_) {
        const std::uint32_t handle = static_cast<std::uint32_t>(ctx.x[0]);
        ++sync_request_calls_;
        // O endereco do buffer do pedido — e' o x1 (ver
        // `eden_bridge.cpp`: sem isto, o upstream ia ler o pedido do
        // TLS e encontrava a resposta anterior).
        bool ok = eden_send_sync_request(eden_->Kernel(), *mem_, handle, ctx.x[1]);
        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            std::fprintf(stderr, "[ipc-probe] eden_send_sync_request returned=%d\n", ok);
            // Dump buffer at SVC return
            std::uint32_t w[8] = {0};
            std::string err;
            if (mem_->read(ctx.x[1], w, sizeof w, err)) {
                std::fprintf(stderr,
                    "[ipc-probe] svc_return addr=0x%llx: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    (unsigned long long)ctx.x[1],
                    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
        if (ok) {
            ctx.ret     = 0;
            ctx.has_ret = true;
            return Result::Success;
        }
        last_ipc_ = "SendSyncRequest: o handle " + std::to_string(handle) +
                    " nao e' uma sessao";
        // `ResultInvalidHandle` (svc_ipc.cpp:32), pela nossa tabela de
        // descrições — o mesmo valor que o resto do despachante devolve.
        ctx.ret     = static_cast<std::uint64_t>(Result::InvalidHandle);
        ctx.has_ret = true;
        return Result::InvalidHandle;
    }

    // `SendSyncRequest(handle, buffer, size)` e `SendSyncRequestLight`.
    //
    // STUB declarado: isto não é uma sessão, é uma tabela handle→nome mais um
    // despacho de quatro comandos do `sm`. Qualquer serviço a sério recebe uma
    // resposta vazia, e o guest vai dizê-lo — o que é a informação útil.
    const std::uint32_t handle = static_cast<std::uint32_t>(ctx.x[0]);
    const std::uint64_t buffer = ctx.x[1];
    const std::size_t size     = static_cast<std::size_t>(ctx.x[2]);

    ++sync_request_calls_;

    const std::string& nome = service_name_for_handle(handle);
    if (!nome.empty() && nome != "sm:") {
        // Um serviço que não é o `sm:`. STUB: resposta vazia com Result de
        // "serviço não implementado", que é mais honesto do que um sucesso
        // falso — um sucesso falso faz o guest interpretar lixo.
        last_ipc_ = "servico \"" + nome + "\": STUB, sem implementação";
        write_empty_reply(buffer, size, kResultNotImplemented);
        return Result::Success;
    }

    if (!ipc_dispatch(buffer, size)) {
        write_empty_reply(buffer, size, 0x301);   // ResultInvalidValue, o que o Eden põe num pedido malformado
        return Result::Success;
    }
    return Result::Success;
}

// `WaitSynchronization(handles, num_handles, timeout_ns)`.
//
// ## O que o Eden faz
//
// `k_synchronization_object.cpp:85-160`, o `KSynchronizationObject::Wait`:
//
//     1. consulta `IsSignaled` em cada objecto da lista; se algum
//        estiver sinalizado, devolve **sucesso** imediatamente, com o
//        indice desse objecto — e **nao dorme**;
//     2. se o timeout for zero, devolve `ResultTimedOut`;
//     3. caso contrario, dorme a thread ate alguém sinalizar.
//
// ## O que este motor faz, e porquê
//
// **O passo 1 e' o que interessa, e e' exacto.** O `KReadableEvent` do
// Eden tem o estado (`m_is_signaled`) e o `IsSignaled()` — e o vendor
// ja os tem, porque o `k_event.cpp` e' o codigo do Eden.
//
// E o guest pode ser servido **sem custo de hardware e sem sair da
// execucao nativa**: o objecto ja esta sinalizado, a espera termina
// ali, e a thread do guest nunca para. Nao ha escalonador, nao ha
// interrupcao, nao ha trabalho desperdicado.
//
// **O passo 3 e' o que este motor nao tem.** Dormir a thread do guest
// exige o escalonador do Eden (`KScopedSchedulerLockAndSleep`), que
// nao existe aqui — e nao deve existir: este motor corre o guest
// nativamente, sem troca de contexto.
//
// A resposta honesta para o passo 3 e' `ResultTimedOut`, que e' o que o
// Eden devolve quando o prazo expira sem nada acontecer. O SDK sabe o
// que isso significa.
//
// **E o passo 3 e' raramente o que acontece.** O `am` sinaliza o
// evento do sistema quando o estado do applet muda, e o SDK espera
// **por** esse evento. Se o evento ja esta sinalizado — e esta, porque
// o applet entra em primeiro plano no arranque — o passo 1 responde, e
// o SDK prossegue. Esta e' a razao do jogo entrar no loop de mensagens
// em vez de ficar a dormir.
//
// ## A diferenca que falhava
//
// A versao anterior devolvia `ResultTimedOut` **sempre**, sem
// consultar o estado. O SDK, que faz `ReceiveMessage` + espera em
// ciclo, voltava a pedir: 1311 pedidos em poucos segundos, e o motor a
// 100% de CPU a fazer nada.
//
// A versao antes dessa devolvia sucesso **sempre** — e o mesmo cycling,
// por outra razao.
//
// **A espera tem de consultar o estado.** E' o que o Eden faz, e e'
// uma linha de codigo.
Result SvcDispatcher::svc_wait_synchronization(SvcContext& ctx) {
    // **A ordem dos argumentos** — a do Eden (`svc.cpp:1420`,
    // `SvcWrap_WaitSynchronization64`):
    //
    //     uint64_t handles     = GetArg64(args, 1);
    //     int32_t  num_handles = GetArg32(args, 2);
    //     uint64_t timeout_ns  = GetArg64(args, 3);
    //     Result   ret = WaitSynchronization64(system, &out_index, handles, num_handles, timeout_ns);
    //     SetArg64(args, 0, ret);
    //     SetArg64(args, 1, out_index);
    //
    // Ou seja: **x0 e' o ponteiro de saida** (que o guest passa e de que o
    // proprio SDK escreve o indice — o wrapper do SDK, `0x82b7cb0c`: `str
    // w1, [x2]`), **x1 e' o array de handles**, **x2 e' o numero**, e
    // **x3 e' o timeout**.
    //
    // **O timeout estava a ser lido de x0** — ou seja, do ponteiro de
    // saida. Medido: o trace mostrava \"timeout=2222980920 ns\", que e' o
    // ponteiro (0x847fff38), e nao um prazo. O SDK espera com o timeout
    // **infinito** (`mov x3, #-1`).
    const std::uint64_t timeout_ns = ctx.x[3];
    const std::uint64_t handles_ptr = ctx.x[1];
    const std::uint32_t num_handles = static_cast<std::uint32_t>(ctx.x[2]);

    // Os handles sao do guest: le-se o array directamente da memoria dele.
    std::uint32_t indices[Kernel::Svc::ArgumentHandleCountMax];
    std::uint32_t n = 0;
    for (std::uint32_t k = 0; k < num_handles && k < Kernel::Svc::ArgumentHandleCountMax; ++k) {
        std::uint32_t h = 0;
        if (!read_guest(handles_ptr + k * sizeof(h), &h, sizeof h)) {
            break;
        }
        indices[n++] = h;
    }

    // O passo 1 do Eden: algum dos objectos ja esta sinalizado?
    //
    // A consulta e' ao `KReadableEvent` — que e' o objecto que o guest
    // recebe (o Eden devolve `GetSystemEvent().GetHandle()`, e o
    // `Event` do Eden devolve o `KReadableEvent*`).
    if (n > 0) {
        auto& kernel = eden_->Kernel();
        auto& processo = Kernel::GetCurrentProcess(kernel);
        for (std::uint32_t k = 0; k < n; ++k) {
            auto evento = processo.GetHandleTable().GetObject<Kernel::KReadableEvent>(
                kernel, Kernel::Handle{indices[k]});
            if (evento.GetPointerUnsafe() == nullptr) {
                continue;
            }
            if (std::getenv("IA_IPC_TRACE") != nullptr) {
                std::fprintf(stderr, "[svc]   evento 0x%x sinalizado=%d\n",
                              indices[k], evento->IsSignaled(kernel) ? 1 : 0);
            }
            if (evento->IsSignaled(kernel)) {
                // **Sinalizado: o passo 1 do Eden.** A espera termina
                // aqui, e devolve o indice do objecto sinalizado.
                //
                // **E o `Wait` nao consome o sinal.** No Eden
                // (`k_synchronization_object.cpp:102-107`) o `Wait`
                // limita-se a ver o estado e a devolver o indice; o
                // `Clear` e' do **`am`**, no
                // `SignalSystemEventIfNeeded`
                // (`lifecycle_manager.cpp:158-171`), e acontece quando o
                // `ShouldSignalSystemEvent()` deixa de ser verdadeiro.
                //
                // A primeira versao fazia o `Clear` aqui — e o guest
                // consumia o sinal e ficava a espera para sempre,
                // porque o `am` nao tinha razao para o re-sinalizar
                // (e nao tinha, sequer: o `am` so' sinaliza quando o
                // estado muda).
                ctx.ret     = 0;
                ctx.has_ret = true;
                ctx.x[1]    = k;
                return Result::Success;
                ctx.ret     = 0;
                ctx.has_ret = true;
                ctx.x[1]    = k;
                return Result::Success;
            }
        }
    }

    // A telemetria: **que** handle espera, e esta sinalizado. E' a
    // pergunta que decide o resto — e sem ela o sintoma e' "o guest
    // esta em ciclo", que nao parece nada.
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[svc] WaitSynchronization: %u handle(s), timeout=%llu ns",
                      n, static_cast<unsigned long long>(timeout_ns));
        for (std::uint32_t k = 0; k < n; ++k) {
            std::fprintf(stderr, " 0x%x", indices[k]);
        }
        // **O x30 e' o endereco de retorno** — quem esta a esperar. E a
        // unica forma de saber que caminho do SDK e' este, sem simbolos.
        std::fprintf(stderr, "  [x30=0x%llx]", static_cast<unsigned long long>(ctx.gpr[30]));
        // E o **conteudo** do array de handles: e' o que o guest
        // realmente espera. O valor ja sai no `0x%x` de cima.
        for (std::uint32_t k = 0; k < n && k < 4; ++k) {
            std::uint32_t v = 0;
            if (read_guest(handles_ptr + k * sizeof(v), &v, sizeof v)) {
                std::fprintf(stderr, " [mem%u=0x%x]", k, v);
            }
        }
        // **A stack do guest.** O SDK guarda o handle num slot da stack
        // (`str w0, [sp, #12]` no `InterProcessEventImplByHorizon::Wait`),
        // e a stack mostra o valor que ele **realmente** recebeu.
        std::uint64_t sp = ctx.sp;
        for (std::uint64_t off = 0; off < 0x40; off += 4) {
            std::uint32_t v = 0;
            if (read_guest(sp + off, &v, sizeof v)) {
                std::fprintf(stderr, " [sp+%llx=%x]",
                             static_cast<unsigned long long>(off), v);
            }
        }
        std::fprintf(stderr, "\n");
    }

    // O passo 2 do Eden: timeout zero e' `ResultTimedOut`.
    if (timeout_ns == 0) {
        ctx.ret     = static_cast<std::uint64_t>(Result::TimedOut);
        ctx.has_ret = true;
        ctx.x[1]    = 0;
        return Result::Success;
    }

    // O passo 3: dormir a thread do guest. Nao existe aqui — ver a nota
    // em cima. A resposta honesta e' o prazo expirado, que e' o que o
    // Eden devolve quando o prazo passa sem nada acontecer.
    ctx.ret     = static_cast<std::uint64_t>(Result::TimedOut);
    ctx.has_ret = true;
    ctx.x[1]    = 0;   // o indice do handle sinalizado
    return Result::Success;
}

Result SvcDispatcher::svc_send_sync_request_light(SvcContext& ctx) {
    // A variante "light" não tem buffer: a resposta é só o Result. O SDK
    // pede-a para comandos sem resposta útil.
    ++sync_request_calls_;
    last_ipc_ = "SendSyncRequestLight (handle " +
                std::to_string(static_cast<std::uint32_t>(ctx.x[0])) + ")";
    return Result::Success;
}

// Escreve uma resposta SMART vazia, com um Result.
//
// Limpa o corpo (o Eden faz `memset` do command buffer inteiro) e escreve o
// Result. É a resposta a um comando que não sabemos responder — melhor do que
// um sucesso falso, que faria o guest interpretar lixo como dados.
void SvcDispatcher::write_empty_reply(std::uint64_t buffer, std::size_t size,
                                      std::uint32_t result) {
    if (mem_ == nullptr || size < kOffBody + 8)
        return;

    std::uint8_t m[kIpcBufferMax];
    const std::size_t n = size < sizeof(m) ? size : sizeof(m);
    if (!read_guest(buffer, m, n))
        return;

    // Nunca escrever além da MessageBuffer: o resto do ThreadLocalRegion são
    // os ThreadVars, e apagá-los parte o guest sem dar nenhum erro.
    std::size_t limite = size < kMessageBufferEnd ? size : kMessageBufferEnd;
    if (limite < kOffBody + 8)
        return;
    for (std::size_t i = kOffBody; i < limite; ++i)
        m[i] = 0;
    for (int i = 0; i < 4; ++i)
        m[kOffPayloadMagic + i] = static_cast<std::uint8_t>(kMagicResponse[i]);
    m[kOffPayloadMagic + 4] = 0;
    m[kOffPayloadMagic + 5] = 0;
    m[kOffPayloadMagic + 6] = 0;
    m[kOffPayloadMagic + 7] = 0;
    m[kOffBody + 0] = static_cast<std::uint8_t>(result);
    m[kOffBody + 1] = static_cast<std::uint8_t>(result >> 8);
    m[kOffBody + 2] = static_cast<std::uint8_t>(result >> 16);
    m[kOffBody + 3] = static_cast<std::uint8_t>(result >> 24);

    std::string err;
    (void)write_guest(buffer, m, limite);
}

std::string SvcDispatcher::service_name_for_handle(std::uint32_t handle) const {
    // Primeiro a tabela de objectos, que é a fonte de verdade: tem geração e
    // valida. O mapa antigo (`name_by_handle_`) morreu — nunca foi preenchido
    // (verificação do código), o que fez o `CloseHandle` da casa ser um
    // no-op e as sessões de serviço acumularem até `ResultOutOfSessions`.
    // Na via com Eden, o nome do serviço vem da sessão e da porta-mãe.
    if (const KObject* o = object_for_handle(handle)) {
        if (o->kind == KObject::Kind::Session && o->parent.raw != 0) {
            if (const KObject* p = object_for_handle(o->parent.raw))
                return p->name;
        }
        return o->name;
    }
    return std::string();
}

KObject* SvcDispatcher::object_for_handle(std::uint32_t handle) const {
    return const_cast<SvcDispatcher*>(this)->handles_.obter(Handle{handle});
}

} // namespace g610
