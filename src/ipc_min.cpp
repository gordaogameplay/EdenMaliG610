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

#include "ia/svc_dispatcher.hpp"

namespace ia {
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
constexpr std::uint32_t kOffBody          = 0x28;

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
constexpr std::uint32_t kReplyDataSize = 8;

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

    // O pedido é do tipo certo? Não responder a um pedido que não é um pedido
    // é pior do que não responder.
    const std::uint32_t type = le32(kOffCommandHeader) & 0xFFFF;
    if (type != kCmdRequestWithContext) {
        last_ipc_ = "tipo de comando inesperado: " + std::to_string(type);
        return false;
    }

    const std::uint32_t magic = le32(kOffPayloadMagic);
    if (magic != kSfcIMagic) {
        last_ipc_ = "magic SFCI ausente (0x" + std::to_string(magic) + ")";
        return false;
    }

    // `data_size` diz quanto do buffer é realmente do IPC. Vem de
    // `CommandHeader.raw_high` bits [0:10] (Eden, ipc.h:58-61), e no pedido
    // real do jogo vale 10 — muito menos do que os 0x1000 que o SDK passa
    // como tamanho do buffer.
    const std::uint32_t raw_high  = le32(kOffCommandHeader + 4);
    const std::uint32_t data_size = raw_high & 0x3FFu;

    // Limite de escrita: o fim da IPC dentro do buffer, e nunca além da
    // MessageBuffer.
    std::size_t limite = kOffBody + data_size;
    if (limite > kMessageBufferEnd)
        limite = kMessageBufferEnd;
    if (limite > n)
        limite = n;
    if (limite < kOffBody + 8)
        limite = (n < kOffBody + 8 ? n : kOffBody + 8);

    const std::uint32_t cmd_id = le32(kOffBody);

    // ── resposta ────────────────────────────────────────────────────────
    // Reconstruir como o `ResponseBuilder` faz: o MESMO `CommandHeader` (o
    // tipo é ecoado do pedido), o magic, e o corpo.
    //
    // O corpo tem de ser limpo. O Eden faz `memset(cmdbuf, 0, ...)` antes de
    // escrever, e sem isso sobram bytes do pedido — que é como um guest
    // descobre que a resposta é feita de remendos.
    for (std::size_t i = kOffBody; i < limite; ++i)
        m[i] = 0;

    // O `CommandHeader` da resposta: `type = 0`.
    //
    // `ResponseBuilder` (ipc_helpers.h:64-67) só escreve `header.type` no
    // caminho TIPC. No CMIF o cabeçalho fica com o valor zero do
    // `CommandHeader header{}`. E `data_size` passa a ser
    // `normal_params + sizeof(DataPayloadHeader)/4 + 4` — os 4 u32 são os
    // 16 bytes de padding obrigatório que o comentário menciona.
    put32(kOffCommandHeader, 0);
    put32(kOffCommandHeader + 4, kReplyDataSize);

    for (int i = 0; i < 4; ++i)
        m[kOffPayloadMagic + i] = static_cast<std::uint8_t>(kMagicResponse[i]);
    put32(kOffPayloadMagic + 4, 0);
    put32(kOffBody, 0);                        // o Result
    put32(kOffBody + 4, 0);                    // sem handles

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
            const char c = static_cast<char>(m[kOffBody + 8 + k]);
            if (c >= ' ' && c <= '~')
                nome.push_back(c);
        }
        last_ipc_ = "sm: GetService \"" + nome + "\"";

        if (nome.empty()) {
            // Nome vazio: o Eden devolve ResultInvalidServiceName em vez de
            // adivinhar. É o honesto — e é o que o guest sabe tratar.
            put32(kOffBody, kSmResultInvalidServiceName);
            break;
        }

        const std::uint32_t h = connect_named_port(nome);
        put32(kOffBody, 0);
        put32(kOffBody + 4, h);
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
        put32(kOffBody, 0x202);
        break;
    }

    std::string err;
    if (!write_guest(buffer, m, limite)) {
        last_ipc_ += "  [ESCRITA FALHOU]";
        return false;
    }
    return true;
}

Result SvcDispatcher::svc_send_sync_request(SvcContext& ctx) {
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
    auto it = name_by_handle_.find(handle);
    return it == name_by_handle_.end() ? std::string() : it->second;
}

} // namespace ia