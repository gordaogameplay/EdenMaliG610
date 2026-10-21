// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
// O **compositor** — o lado do *binder* que recebe os buffers do jogo.
//
// ## O que é
//
// O jogo desenha através do *binder* do Android (o `IGraphicBufferProducer`),
// e o *binder* é o serviço que o **compositor** atende. Na casa, o
// compositor é este ficheiro.
//
// O jogo:
//
//   1. liga-se (`Connect`);
//   2. **prealoca** os buffers e entrega-os (`SetPreallocatedBuffer`) —
//      cada um com o **handle do `nvmap`**, que é o endereço onde o
//      buffer vive;
//   3. pede um buffer (`DequeueBuffer`) e recebe o número do *slot*;
//   4. desenha lá dentro;
//   5. devolve-o (`QueueBuffer`) — e é aqui que o compositor tem o
//      desenho.
//
// ## A forma é a do Eden, byte a byte
//
// O *parcel* (`nvnflinger/parcel.h`): um cabeçalho de 16 bytes
// (`ParcelHeader`: o tamanho e o offset dos dados, e o dos objectos), os
// dados, e os objectos.
//
// E os **códigos das transacções** são os do Eden
// (`nvnflinger/buffer_queue_producer.cpp`, `Transact`):
//
//   1 = RequestBuffer      2 = SetBufferCount   3 = DequeueBuffer
//   4 = DetachBuffer       5 = DetachNextBuffer 6 = AttachBuffer
//   7 = QueueBuffer        8 = CancelBuffer     9 = Query
//  10 = Connect           11 = Disconnect      13 = AllocateBuffers
//  14 = SetPreallocatedBuffer                    17 = GetBufferHistory
//
// **O `Status` vai no fim** dos dados (é assim que o Eden o escreve) —
// e o Eden escreve-o sempre, mesmo quando a transacção é um `void`.
//
// ## O que a casa implementa
//
// As transacções que o jogo **usa** (medido: 9, 10 e 14) e as que
// precisa para continuar (3 = `DequeueBuffer`, 7 = `QueueBuffer`). As
// restantes respondem com `Status::BadValue` — que é o que o Eden faz
// quando o `IBinder` não conhece a transacção.
//
// **O `QueueBuffer` é o momento em que o desenho do jogo chega à tela**:
// é aqui que o compositor fica com o endereço do buffer desenhado.
//
// SPDX-FileCopyrightText: 2018 Yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/hle/kernel/k_process_bridge.h"
#include "g610/services/nvmap_registry.hpp"

namespace g610::services {

namespace {

// Despeja a native window — o diagnostico de "o jogo desenhou?".
void DespejarJanela();
void VarrerRegiaoDoCanal();

} // namespace

void DespejarJanelaDoCompositor() {
    DespejarJanela();
}

void VarrerRegiaoDoCanalDoCompositor() {
    VarrerRegiaoDoCanal();
}

namespace {

// Ler memória do guest — a mesma leitura que o `nvdrv` faz (o VA da GPU
// é o espaço do guest).
//
// O processo corrente é o da **ponte** (`Bridge::processo()`), que é
// global: é o mesmo que o `nvdrv` usa.
bool LerDoGuest(std::uint64_t endereco, void* destino, std::size_t tamanho) {
    auto* processo = Kernel::Bridge::processo();
    if (processo == nullptr)
        return false;
    return processo->GetMemory().ReadBlock(static_cast<Common::ProcessAddress>(endereco),
                                          destino, tamanho);
}

// ─────────────────────────────────────────────────────────────────────────────
// O `Status` do Android (`nvnflinger/status.h`).
//
// É o que o Eden devolve no fim dos dados. O `NoError` (0) é o caminho
// feliz; o `BadValue` (-22) é o "não sei esta transacção".
// ─────────────────────────────────────────────────────────────────────────────
enum Status : std::int32_t {
    StatusNoError = 0,
    StatusBadValue = -22,
    StatusInvalidOperation = -38,
};

// O `NativeWindow` (`nvnflinger/window.h`): o que se pode perguntar ao
// produtor com a transacção 9.
enum NativeWindow : std::int32_t {
    WindowWidth = 0,
    WindowHeight = 1,
    WindowFormat = 2,
    WindowMinUndequeuedBuffers = 3,
    WindowQueuesToWindowComposer = 4,
    WindowConcreteType = 5,
    WindowDefaultWidth = 6,
    WindowDefaultHeight = 7,
    WindowTransformHint = 8,
    WindowConsumerRunningBehind = 9,
    WindowConsumerUsageBits = 10,
    WindowStickyTransform = 11,
    WindowDefaultDataSpace = 12,
    WindowBufferAge = 13,
};

// O `PixelFormat` (`nvnflinger/pixel_format.h`).
enum PixelFormat : std::uint32_t {
    FormatRgba8888 = 1,
    FormatRgbx8888 = 2,
    FormatBgra8888 = 5,
};

// A resolução que a casa usa (a do Switch) — e que é o que o `Query`
// devolve.
constexpr std::int32_t kLargura = 1280;
constexpr std::int32_t kAltura = 720;

// ─────────────────────────────────────────────────────────────────────────────
// O *parcel*, do Eden (`nvnflinger/parcel.h`).
// ─────────────────────────────────────────────────────────────────────────────

// O cabeçalho (`ParcelHeader`, 16 bytes).
struct ParcelHeader {
    std::uint32_t data_size = 0;
    std::uint32_t data_offset = 0;
    std::uint32_t objects_size = 0;
    std::uint32_t objects_offset = 0;
};
static_assert(sizeof(ParcelHeader) == 16, "tamanho errado");

// O leitor (`InputParcel` do Eden).
//
// **A ordem de leitura é a do Eden**: o cabeçalho, o token de interface
// (um `u32` desconhecido + o comprimento + os caracteres em UTF‑16), e
// depois os valores — cada um alinhado a 4 bytes.
class LeitorParcel {
public:
    explicit LeitorParcel(const std::vector<std::uint8_t>& dados) : dados_(dados) {
        if (dados_.size() >= sizeof(ParcelHeader)) {
            ParcelHeader cabecalho{};
            std::memcpy(&cabecalho, dados_.data(), sizeof(cabecalho));
            indice_ = cabecalho.data_offset;
        }
        TokenDaInterface();
    }

    // O token: o Eden lê um `u32` (o "0x100" que medimos) e o
    // comprimento, e depois (comprimento + 1) caracteres em UTF‑16.
    //
    // **Os caracteres NÃO são alinhados** ✗ — o upstream usa
    // `ReadUnaligned<u16>()` (`parcel.h:96` ✗), que deixa o índice onde
    // está ✗. A casa alinhava a 4 bytes **depois** de cada caracter ✗ ✗.
    //
    // **Medido** ✗ (`/tmp/boot_v17.log` ✗): o token do `android.gui`
    // (11 caracteres ✗ + o terminador ✗ = 12 ✗) deixava o índice em
    // **164** ✗ — quando 16 (o `data_offset` ✗) + 8 (o cabeçalho do token
    // ✗) + 24 (os 12 caracteres ✗) = **48** ✗ ✗. A diferença ✗ — 116
    // bytes ✗ — eram os 2 bytes de desperdício por caracter ✗ ✗.
    //
    // **Consequência medida** ✗: o `SetPreallocatedBuffer` (transacção 14
    // ✗) lia o *slot* ✗ e o *handle* ✗ de sitios errados ✗ — e o
    // `LeObjecto` falhava ✗, o que fazia o compositor devolver
    // `StatusBadValue` ✗ ✗. O jogo ficava sem poder desenhar ✗ ✗.
    void TokenDaInterface() {
        (void)Le<u32>();
        const std::uint32_t comprimento = Le<u32>();
        for (std::uint32_t i = 0; i < comprimento + 1 && TemBytes(2); ++i)
            (void)LeSemAlinhar<std::uint16_t>();
        Alinha();
    }

    template <typename T>
    T Le() {
        T valor{};
        if (TemBytes(sizeof(T))) {
            std::memcpy(&valor, dados_.data() + indice_, sizeof(T));
            indice_ += sizeof(T);
        }
        Alinha();
        return valor;
    }

    // A leitura **sem alinhamento** — o `ReadUnaligned` do upstream
    // (`parcel.h:89-99` ✗). So o token a usa ✗.
    template <typename T>
    T LeSemAlinhar() {
        T valor{};
        if (TemBytes(sizeof(T))) {
            std::memcpy(&valor, dados_.data() + indice_, sizeof(T));
            indice_ += sizeof(T);
        }
        return valor;
    }

    // O objecto "achatado" (`ReadFlattened`): o tamanho, e a estrutura.
    template <typename T>
    bool LeAchatado(T& valor) {
        const std::int64_t tamanho = Le<std::int64_t>();
        if (static_cast<std::size_t>(tamanho) != sizeof(T))
            return false;
        if (!TemBytes(sizeof(T)))
            return false;
        std::memcpy(&valor, dados_.data() + indice_, sizeof(T));
        indice_ += sizeof(T);
        Alinha();
        return true;
    }

    // O objecto (`ReadObject`): uma bandeira, e — se estiver válida — o
    // objecto achatado.
    template <typename T>
    bool LeObjecto(T& valor) {
        const bool valido = Le<std::uint8_t>() != 0;
        if (!valido)
            return false;
        return LeAchatado(valor);
    }

private:
    bool TemBytes(std::size_t n) const { return indice_ + n <= dados_.size(); }
    void Alinha() {
        if (indice_ % 4 != 0)
            indice_ += 4 - (indice_ % 4);
    }

    const std::vector<std::uint8_t>& dados_;
    std::size_t indice_ = 0;
};

// O escritor (`OutputParcel` do Eden).
class EscritorParcel {
public:
    template <typename T>
    void Escreve(T valor) {
        EscreveBytes(&valor, sizeof(T));
    }

    void EscreveBytes(const void* dados, std::size_t n) {
        Alinha();
        const auto* p = static_cast<const std::uint8_t*>(dados);
        dados_.insert(dados_.end(), p, p + n);
    }

    // O objecto achatado (`WriteFlattenedObject`): a bandeira, o
    // tamanho, e a estrutura.
    template <typename T>
    void EscreveAchatado(const T& valor) {
        Escreve<std::uint32_t>(1);
        Escreve<std::int64_t>(sizeof(T));
        EscreveBytes(&valor, sizeof(T));
    }

    std::vector<std::uint8_t> Serializa() const {
        std::vector<std::uint8_t> saida(sizeof(ParcelHeader) + dados_.size() + objectos_.size(), 0);
        ParcelHeader cabecalho{};
        cabecalho.data_size = static_cast<std::uint32_t>(dados_.size());
        cabecalho.data_offset = sizeof(ParcelHeader);
        cabecalho.objects_size = static_cast<std::uint32_t>(objectos_.size());
        cabecalho.objects_offset = cabecalho.data_offset + cabecalho.data_size;
        std::memcpy(saida.data(), &cabecalho, sizeof(cabecalho));
        std::memcpy(saida.data() + cabecalho.data_offset, dados_.data(), dados_.size());
        std::memcpy(saida.data() + cabecalho.objects_offset, objectos_.size() ? objectos_.data() : saida.data(),
                    objectos_.size());
        return saida;
    }

private:
    void Alinha() {
        if (dados_.size() % 4 != 0)
            dados_.insert(dados_.end(), 4 - (dados_.size() % 4), 0);
    }

    std::vector<std::uint8_t> dados_;
    std::vector<std::uint8_t> objectos_;
};

// ─────────────────────────────────────────────────────────────────────────────
// A fila de buffers.
//
// O `NvGraphicBuffer` do Eden (`nvnflinger/ui/graphic_buffer.h`, 0x16C
// bytes): o `handle` está no **offset 116** e é o handle do `nvmap` —
// o handle é que o compositor usa para chegar ao endereço.
// ─────────────────────────────────────────────────────────────────────────────
struct NvGraphicBuffer {
    std::uint8_t bytes[0x16C]{};
    // O `buffer_id` (offset 44 ✗) e' o **handle do `nvmap`** ✗ — e e'
    // ele que o compositor usa para chegar ao endereco ✗ ✗.
    //
    // **O upstream** (`nvnflinger/ui/graphic_buffer.cpp:23-25` ✗) usa
    // `BufferId()` ✗ — o `nvmap->DuplicateHandle(this->BufferId(), true)` ✗
    // e o `PinHandle` ✗ ✗. O `Handle()` (offset 116 ✗) e' o handle do
    // **buffer de video** ✗ — nao do `nvmap` ✗ ✗.
    //
    // **A casa usava o `Handle()`** ✗ ✗ — o que dava o handle do `nvmap`
    // errado ✗ (e o endereco errado ✗) ✗.
    static constexpr std::size_t kOffsetBufferId = 44;
    static constexpr std::size_t kOffsetHandle = 116;

    [[nodiscard]] std::uint32_t BufferId() const {
        std::uint32_t h = 0;
        std::memcpy(&h, bytes + kOffsetBufferId, sizeof(h));
        return h;
    }

    [[nodiscard]] std::uint32_t Handle() const {
        std::uint32_t h = 0;
        std::memcpy(&h, bytes + kOffsetHandle, sizeof(h));
        return h;
    }

    // A largura ✗ — o offset 4 ✗ (`nvnflinger/ui/graphic_buffer.h` ✗).
    [[nodiscard]] std::uint32_t Width() const {
        std::int32_t w = 0;
        std::memcpy(&w, bytes + 4, sizeof(w));
        return static_cast<std::uint32_t>(w);
    }

    // A altura ✗ — o offset 8 ✗.
    [[nodiscard]] std::uint32_t Height() const {
        std::int32_t h = 0;
        std::memcpy(&h, bytes + 8, sizeof(h));
        return static_cast<std::uint32_t>(h);
    }

    // O formato ✗ — o offset 16 ✗.
    [[nodiscard]] std::uint32_t Format() const {
        std::uint32_t f = 0;
        std::memcpy(&f, bytes + 16, sizeof(f));
        return f;
    }
};
static_assert(sizeof(NvGraphicBuffer) == 0x16C, "tamanho errado");

// O estado da fila: os *slots* (o Eden tem 16 — `BufferQueueDefs`).
constexpr std::size_t kSlots = 16;

struct Slot {
    bool ocupado = false;        // o jogo já o deu
    std::uint64_t endereco = 0;   // a memória do buffer
    std::uint64_t tamanho = 0;
    bool dequeued = false;       // o jogo está a desenhar nele
    bool desenhado = false;      // o jogo já o devolveu
};

struct FilaDeBuffers {
    Slot slots[kSlots]{};
    // O último buffer devolvido — é o desenho mais recente, e o que o
    // compositor apresenta.
    std::uint64_t ultimo_desenhado = 0;
    std::uint64_t tamanho_ultimo = 0;
    std::uint64_t submissoes = 0;

    // **As dimensoes da fila** ✗ — o upstream (`buffer_queue_core.h:89-91`
    // ✗) tem `default_width` ✗, `default_height` ✗ e
    // `default_buffer_format` ✗, que o `SetPreallocatedBuffer` define ✗
    // (`buffer_queue_producer.cpp:814-816` ✗) e que o `Connect` ✗ e o
    // `Query` ✗ devolvem ✗ ✗.
    //
    // **Antes do `SetPreallocatedBuffer`** ✗, os valores sao **1** ✗ (o
    // upstream ✗ — `default_width{1}` ✗) ✗. A casa devolvia 1280×720 ✗
    // fixos ✗ ✗ — o que era um palpite ✗, nao o upstream ✗ ✗.
    // **Os valores por omissao** ✗ — o upstream
    // (`nvnflinger/buffer_queue_core.h:88-91` ✗):
    //
    //     PixelFormat default_buffer_format{PixelFormat::Rgba8888};
    //     u32 default_width{1};
    //     u32 default_height{1};
    //
    // **O formato por omissao e' `Rgba8888` (=1)** ✗ — e a casa punha
    // **0** ✗ (`NoFormat` ✗) ✗ ✗. O jogo perguntou o formato ✗
    // (`Query` com `o_que=2` ✗) e recebeu `0` ✗ ✗.
    std::uint32_t largura = 1;
    std::uint32_t altura = 1;
    std::uint32_t formato = 1;  // Rgba8888
};

// A **janela** — o buffer onde o jogo desenha (a native window).
//
// É a mesma reserva que o `nvdrv` dá ao jogo (o espaço de VA da GPU), e
// é a partir de 0x98000000 — à frente das alocações do `nvmap`.
std::uint64_t g_janela = 0;
std::uint64_t g_tamanho_janela = 0;

// A **regiao do canal da GPU**: varre-a a procura de qualquer coisa
// escrita.
//
// E' a medicao que decide onde esta o desenho: o guest deu ao canal um
// buffer de 313 MB (medido), e a native window de 1280x720 (medido).
// Se a regiao do canal tiver conteudo, e' ali que o jogo desenha.
void VarrerRegiaoDoCanal() {
    RegistoNvmap::Regiao regiao{};
    if (!RegistoNvmap::Instancia().ProcurarCanal(regiao)) {
        LOG_DEBUG(Service_NVDRV, "[canal] o registo nao sabe o canal");
        return;
    }
    constexpr std::uint64_t kBloco = 1ull << 20;  // 1 MiB
    std::vector<std::uint8_t> bloco(static_cast<std::size_t>(kBloco), 0);
    for (std::uint64_t off = 0; off + kBloco <= regiao.tamanho; off += kBloco) {
        if (!LerDoGuest(regiao.endereco + off, bloco.data(), bloco.size()))
            continue;
        bool vazio = true;
        for (std::uint8_t b : bloco) {
            if (b != 0) {
                vazio = false;
                break;
            }
        }
        if (vazio)
            continue;
        // **Escreveu**: mostra o inicio do bloco.
        LOG_DEBUG(Service_NVDRV, "[canal] escrita a partir de +0x{:X} (regiao 0x{:X}, {} bytes)",
                  off, regiao.endereco, regiao.tamanho);
        char linha[256];
        std::size_t n = 0;
        n += static_cast<std::size_t>(
            std::snprintf(linha + n, sizeof(linha) - n, "[canal] +0x%llX:",
                           (unsigned long long)off));
        for (std::size_t i = 0; i < 64 && n + 4 < sizeof(linha); ++i)
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, " %02x", bloco[i]));
        if (n + 2 < sizeof(linha))
            n += static_cast<std::size_t>(std::snprintf(linha + n, sizeof(linha) - n, "\n"));
        const ssize_t w = ::write(2, linha, n);
        (void)w;
        return;
    }
    LOG_DEBUG(Service_NVDRV, "[canal] a regiao de {} bytes esta toda a ZEROS",
              regiao.tamanho);
}

// A native window: os primeiros bytes, em hexadecimal.
void DespejarJanela() {
    if (g_janela == 0)
        return;
    std::uint8_t janela[64] = {0};
    if (!LerDoGuest(g_janela, janela, sizeof(janela)))
        return;
    char linha[256];
    std::size_t n = 0;
    n += static_cast<std::size_t>(
        std::snprintf(linha + n, sizeof(linha) - n, "[janela] 0x%llX:",
                       (unsigned long long)g_janela));
    for (std::size_t i = 0; i < sizeof(janela) && n + 4 < sizeof(linha); ++i)
        n += static_cast<std::size_t>(
            std::snprintf(linha + n, sizeof(linha) - n, " %02x", janela[i]));
    if (n + 2 < sizeof(linha))
        n += static_cast<std::size_t>(std::snprintf(linha + n, sizeof(linha) - n, "\n"));
    const ssize_t w = ::write(2, linha, n);
    (void)w;
}

FilaDeBuffers& Fila() {
    static FilaDeBuffers f;
    return f;
}

} // namespace

std::uint64_t AlocarJanelaDoCompositor(std::uint64_t tamanho) {
    // A janela fica **noutra** zona: o `nvmap` aloca a partir de
    // 0x98000000 (e o guest pediu 313 MB), por isso a native window
    // fica logo acima, em 0x9C800000.
    static std::uint64_t proximo = 0x9C800000ull;
    constexpr std::uint64_t kFim = 0x9CC00000ull;  // 4 MiB
    if (proximo + tamanho > kFim)
        return 0;
    g_janela = proximo;
    g_tamanho_janela = tamanho;
    proximo += (tamanho + 0xFFF) & ~0xFFFull;
    return g_janela;
}

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// O `Fence` do Android (`nvnflinger/ui/fence.h`, 36 bytes): o número de
// fences e quatro `NvFence` (id + valor).
// ─────────────────────────────────────────────────────────────────────────────
struct NvFence {
    std::int32_t id = -1;
    std::uint32_t value = 0;
};

struct Fence {
    std::uint32_t num_fences = 0;
    NvFence fences[4]{};
};
static_assert(sizeof(Fence) == 36, "tamanho errado");

// O `QueueBufferOutput` (`nvnflinger/graphic_buffer_producer.h`, 16 bytes).
struct QueueBufferOutput {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t transform_hint = 0;
    std::uint32_t num_pending_buffers = 0;
};
static_assert(sizeof(QueueBufferOutput) == 16, "tamanho errado");

// O `QueueBufferInput` (`graphic_buffer_producer.h`, 84 bytes) — o que o
// jogo diz ao devolver o buffer.
//
// **É `pack(1)`** — como no Eden (`#pragma pack(push, 1)`): sem isso, o
// alinhamento de 8 bytes do `s64` faria a struct ter 88 bytes, e o
// Eden diz 84.
#pragma pack(push, 1)
struct QueueBufferInput {
    std::int64_t timestamp = 0;
    std::int32_t is_auto_timestamp = 0;
    std::int32_t crop[4]{};
    std::int32_t scaling_mode = 0;
    std::uint32_t transform = 0;
    std::uint32_t sticky_transform = 0;
    std::int32_t async = 0;
    std::int32_t swap_interval = 0;
    Fence fence{};
};
#pragma pack(pop)
static_assert(sizeof(QueueBufferInput) == 84, "tamanho errado");

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// A transacção.
//
// Devolve o *parcel* de resposta — o Eden escreve-o no buffer que o
// chamador passou, e o formato é o do `OutputParcel`.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::uint8_t> TransactionarParcel(std::uint32_t codigo,
                                               const std::vector<std::uint8_t>& entrada) {
    LeitorParcel leitor(entrada);
    EscritorParcel escritor;

    // O `Status` — **o Eden escreve-o no fim**, sempre
    // (`buffer_queue_producer.cpp`, o fim do `Transact`).
    std::int32_t status = StatusNoError;

    switch (codigo) {
    case 9: {  // Query
        const std::int32_t o_que = leitor.Le<std::int32_t>();
        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            std::fprintf(stderr, "[binder] Query: o_que=%d\n", o_que);
        }
        std::int32_t valor = 0;
        switch (o_que) {
        case WindowWidth:
        case WindowDefaultWidth:
            valor = static_cast<std::int32_t>(Fila().largura);
            break;
        case WindowHeight:
        case WindowDefaultHeight:
            valor = static_cast<std::int32_t>(Fila().altura);
            break;
        case WindowFormat:
            valor = static_cast<std::int32_t>(Fila().formato);
            break;
        case WindowMinUndequeuedBuffers:
            valor = 2;
            break;
        case WindowQueuesToWindowComposer:
            valor = 1;
            break;
        case WindowConcreteType:
            valor = 0;
            break;
        case WindowTransformHint:
        case WindowStickyTransform:
            valor = 0;
            break;
        case WindowConsumerRunningBehind:
            valor = 0;
            break;
        case WindowConsumerUsageBits:
            valor = 0;
            break;
        case WindowDefaultDataSpace:
            valor = 0;
            break;
        case WindowBufferAge:
            valor = 0;
            break;
        default:
            status = StatusBadValue;
            break;
        }
        escritor.Escreve<std::int32_t>(valor);
        break;
    }
    case 10: {  // Connect
        (void)leitor.Le<std::uint8_t>();   // o enable_listener
        const std::int32_t api = leitor.Le<std::int32_t>();  // o api
        (void)leitor.Le<std::uint8_t>();   // o producer_controlled_by_app

        // **A `api` tem de ser uma das quatro** ✗ — o upstream
        // (`buffer_queue_producer.cpp:716-726` ✗) so' aceita `Egl` ✗,
        // `Cpu` ✗, `Media` ✗ e `Camera` ✗; o resto e' `Status::BadValue`
        // ✗ ✗.
        //
        // A casa aceitava qualquer uma ✗ ✗ — e devolvia a saida ✗ como
        // se fosse valida ✗ ✗.
        if (api < 1 || api > 4) {
            status = StatusBadValue;
            break;
        }

        QueueBufferOutput saida{};
        // **As dimensoes da fila** ✗ — o upstream (`buffer_queue_producer.cpp:723`
        // ✗) devolve o `default_width` ✗ e o `default_height` ✗ ✗.
        saida.width = Fila().largura;
        saida.height = Fila().altura;
        saida.transform_hint = 0;
        saida.num_pending_buffers = 0;
        escritor.EscreveBytes(&saida, sizeof(saida));
        break;
    }
    case 14: {  // SetPreallocatedBuffer
        const std::int32_t slot = leitor.Le<std::int32_t>();
        NvGraphicBuffer buffer{};
        if (!leitor.LeObjecto(buffer)) {
            status = StatusBadValue;
            break;
        }
        // O handle é o do `nvmap` — e é por ele que se chega ao
        // endereço.
        RegistoNvmap::Regiao regiao{};
        if (!RegistoNvmap::Instancia().Procurar(buffer.BufferId(), regiao)) {
            LOG_WARNING(Service_NVDRV,
                        "compositor: SetPreallocatedBuffer: buffer_id {} desconhecido",
                        buffer.BufferId());
            status = StatusBadValue;
            break;
        }
        if (slot < 0 || static_cast<std::size_t>(slot) >= kSlots) {
            status = StatusBadValue;
            break;
        }
        auto& s = Fila().slots[slot];
        s.ocupado = true;
        s.endereco = regiao.endereco;
        s.tamanho = regiao.tamanho;
        // **As dimensoes** ✗ — o upstream (`buffer_queue_producer.cpp:814-816`
        // ✗) guarda-as no `BufferQueueCore` ✗, e o `Connect` ✗ e o `Query`
        // ✗ devolvem-nas ✗ ✗.
        Fila().largura = buffer.Width();
        Fila().altura = buffer.Height();
        Fila().formato = buffer.Format();
        LOG_DEBUG(Service_NVDRV,
                  "compositor: SetPreallocatedBuffer: slot={} buffer_id={} -> 0x{:X} "
                  "({} bytes)",
                  slot, buffer.BufferId(), regiao.endereco, regiao.tamanho);
        break;
    }
    case 3: {  // DequeueBuffer
        (void)leitor.Le<std::uint8_t>();    // o is_async
        (void)leitor.Le<std::uint32_t>();   // a largura
        (void)leitor.Le<std::uint32_t>();   // a altura
        (void)leitor.Le<std::uint32_t>();   // o formato
        (void)leitor.Le<std::uint32_t>();   // o uso

        // O slot: o primeiro que o jogo deu e que não está em uso.
        std::int32_t slot = -1;
        for (std::size_t i = 0; i < kSlots; ++i) {
            if (Fila().slots[i].ocupado && !Fila().slots[i].dequeued) {
                slot = static_cast<std::int32_t>(i);
                break;
            }
        }
        if (slot < 0) {
            status = StatusBadValue;
            break;
        }
        Fila().slots[slot].dequeued = true;
        LOG_DEBUG(Service_NVDRV, "compositor: DequeueBuffer: slot={}", slot);

        escritor.Escreve<std::int32_t>(slot);
        Fence fence{};  // sem fence: o desenho é síncrono
        escritor.EscreveAchatado(fence);
        break;
    }
    case 7: {  // QueueBuffer
        const std::int32_t slot = leitor.Le<std::int32_t>();
        QueueBufferInput entrada_buffer{};
        (void)leitor.LeAchatado(entrada_buffer);

        if (slot < 0 || static_cast<std::size_t>(slot) >= kSlots ||
            !Fila().slots[slot].ocupado) {
            status = StatusBadValue;
            break;
        }
        auto& s = Fila().slots[slot];
        s.dequeued = false;
        s.desenhado = true;
        Fila().ultimo_desenhado = s.endereco;
        Fila().tamanho_ultimo = s.tamanho;
        Fila().submissoes += 1;
        LOG_DEBUG(Service_NVDRV,
                  "compositor: QueueBuffer: slot={} desenho em 0x{:X} ({} bytes)", slot,
                  s.endereco, s.tamanho);

        // **A janela, em hexadecimal.** É aqui que o desenho tem de
        // estar: se isto não tiver pixels, o jogo não desenhou (e o
        // caminho da GPU não é o que pensávamos).
        {
            std::uint8_t janela[64] = {0};
            if (LerDoGuest(g_janela, janela, sizeof(janela))) {
                char linha[256];
                std::size_t n = 0;
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, "[janela] 0x%llX:",
                                   (unsigned long long)g_janela));
                for (std::size_t i = 0; i < sizeof(janela) && n + 4 < sizeof(linha); ++i)
                    n += static_cast<std::size_t>(
                        std::snprintf(linha + n, sizeof(linha) - n, " %02x", janela[i]));
                if (n + 2 < sizeof(linha))
                    n += static_cast<std::size_t>(
                        std::snprintf(linha + n, sizeof(linha) - n, "\n"));
                const ssize_t w = ::write(2, linha, n);
                (void)w;
            }
        }
        break;
    }
    case 2: {  // SetBufferCount
        (void)leitor.Le<std::int32_t>();
        break;
    }
    case 8: {  // CancelBuffer
        (void)leitor.Le<std::int32_t>();
        Fence fence{};
        (void)leitor.LeAchatado(fence);
        break;
    }
    default:
        LOG_WARNING(Service_NVDRV, "compositor: transacção {} não implementada", codigo);
        status = StatusBadValue;
        break;
    }

    // O `Status` no fim — como o Eden.
    escritor.Escreve<std::int32_t>(status);
    if (std::getenv("IA_IPC_TRACE") != nullptr) {
        std::fprintf(stderr, "[binder] transaccao %u -> status=%d\\n", codigo, status);
    }
    return escritor.Serializa();
}

} // namespace g610::services
