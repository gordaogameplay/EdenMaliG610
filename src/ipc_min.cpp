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

#include <cstdlib>

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

    // Onde está o payload? Não é uma constante — é uma ARITMÉTICA que depende
    // dos descritores que o pedido traz.
    //
    // Medido nos dois pedidos reais do jogo:
    //
    //   pedido 1 (Initialize): CommandHeader + BufferDescriptorC +
    //                           HandleDescriptorHeader  -> payload em 0x20
    //   pedido 2 (GetService "lm"): CommandHeader + um bloco de 8 bytes
    //                           -> payload em 0x10
    //
    // Duas tentativas com uma constante fixa (0x28, depois 0x18) falharam as
    // duas, e era porque o valor não é fixo.
    //
    // Em vez de Reproduzir a aritmética dos descritores — que tem três casos
    // e um alinhamento de 16 bytes — procura-se a assinatura. É mais barato,
    // é à prova de um descritor que não conhecemos, e se um dia aparecer uma
    // geometria nova o código continua a funcionar em vez de escrever por
    // cima do sítio errado.
    std::size_t payload = 0;
    bool achou = false;
    for (std::size_t k = 0; k + 4 <= n && k < kPayloadScanMax; k += 4) {
        if (le32(k) == kSfcIMagic) {
            payload = k;
            achou = true;
            break;
        }
    }
    if (!achou) {
        last_ipc_ = "assinatura SFCI nao encontrada nos primeiros " +
                    std::to_string(kPayloadScanMax) + " bytes";
        return false;
    }

    const std::size_t cmd_off = payload + 4;   // cmd_id logo apos o magic
    const std::size_t name_off = payload + 0x10;  // o nome do servico

    if (name_off + 8 > n) {
        last_ipc_ = "pedido truncado antes do nome do servico";
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
    const std::size_t pay_r = v_payload ? (payload + 0x10) : payload;

    std::size_t lim_r = pay_r + kReplyDataSize + 4;
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

    for (int i = 0; i < 4; ++i)
        m[pay_r + i] = static_cast<std::uint8_t>(kMagicResponse[i]);
    put32(pay_r + 4, 0);                       // o Result

    if (v_handles) {
        // O handle vai no descritor: logo a seguir ao `CommandHeader`, como o
        // `ResponseBuilder` faz (`ctx.handles_offset = index` logo depois de
        // escrever o `HandleDescriptorHeader`).
        put32(kOffCommandHeader + 8, 0);       // HandleDescriptorHeader
        put32(kOffCommandHeader + 12, 0);      // o handle movido
    } else {
        put32(pay_r + 8, 0);                   // sem handles
    }

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
            const char c = static_cast<char>(m[name_off + k]);
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
            put32(pay_r + 4, kSmResultInvalidServiceName);
            break;
        }

        const std::uint32_t h = connect_named_port(nome);
        put32(pay_r + 4, 0);
        if (v_handles)
            put32(kOffCommandHeader + 12, h);
        else
            put32(pay_r + 8, h);
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
        put32(pay_r + 4, 0x202);
        break;
    }

    last_ipc_ += "  [variante " + std::to_string(variante) +
                (v_handles ? ": handle no descritor" : ": handle inline") +
                (v_tipo ? ", type ecoado" : ", type=0") +
                (v_payload ? ", payload+0x10" : ", payload ecoado") +
                (v_silencio ? ", SEM RESPOSTA" : "") + "]";

    if (v_silencio) {
        // Controlo: nao escreve nada. Se o guest tambem falhar com isto, o
        // problema nao e o conteudo da resposta.
        return true;
    }

    std::string err;
    if (!write_guest(buffer, m, lim_r)) {
        last_ipc_ += "  [ESCRITA FALHOU]";
        return false;
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