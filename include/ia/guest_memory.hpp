// EdenMaliIA — espaço de endereçamento do guest.
//
// O guest de 39 bits corre NATIVO no A76: os endereços que ele calcula são os
// endereços reais que o CPU usa. Não há tradução de endereços nem page tables
// nossas — o que existe é uma reserva de espaço de endereçamento com MAP_FIXED
// no sítio certo, para que o código do jogo encontre o que espera.
//
// Isto é o "fastmem" de que o NCE depende. Não é uma optimização: sem um
// mapeamento no endereço certo, o primeiro acesso do guest a um endereço
// calculado pelo linker dá SIGSEGV.
//
// ---------------------------------------------------------------------------
// O QUE O GUEST ESPERA (medido, não suposto)
// ---------------------------------------------------------------------------
// Num NSO, os vaddr dos segmentos são relativos ao base do módulo. No `main`
// do Pokémon Shield:
//
//   text    vaddr=0x00000000  size=25084672
//   rodata  vaddr=0x017ed000  <- 3328 bytes de gap
//   data    vaddr=0x0236d000  <- 784 bytes de gap
//
// E o base do processo 39-bit é 0x8000'0000 (src/core/hle/kernel/k_process.cpp
// do Eden), não o topo dos 39 bits. Ver `NsoLoader::default_base`.
//
// ---------------------------------------------------------------------------
// O QUE O HOST PERMITE
// ---------------------------------------------------------------------------
// Medido no Orange Pi 5 Pro (AArch64, kernel 6.18 mainline):
//
//   MAP_FIXED_NOREPLACE em 0x8000'0000  -> OK
//   espaço de utilizador até            -> 0x0000_7FFF_FFFF_F000 (47 bits)
//
// Portanto 39 bits cabem folgadamente. O que NÃO cabe é 48 bits em alguns
// kernels — e por isso isto é verificado em vez de assumido.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ia {

// O espaço do guest é mapeado com permissões por região, não com RWX global.
// Separar é o que permite ao patcher escrever no .text sem dar permissão de
// escrita ao código do guest em execução.
enum class Prot : std::uint8_t {
    Read = 1 << 0,
    Write = 1 << 1,
    Exec = 1 << 2,
};

constexpr Prot operator|(Prot a, Prot b) noexcept {
    return static_cast<Prot>(static_cast<std::uint8_t>(a) |
                             static_cast<std::uint8_t>(b));
}

// Converte para as flags do mmap.
int to_posix(Prot p) noexcept;

// ---------------------------------------------------------------------------
// GuestMemory — a reserva do espaço de endereçamento do guest.
// ---------------------------------------------------------------------------
//
// Uma instância por processo guest. Não é copiável: guarda o mapeamento real,
// e copiar a estrutura sem o map seria um bug silencioso.
class GuestMemory {
public:
    GuestMemory() = default;
    ~GuestMemory();

    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    GuestMemory(GuestMemory&& other) noexcept;
    GuestMemory& operator=(GuestMemory&& other) noexcept;

    // Reserva [base, base+size) com as permissões dadas.
    //
    // MAP_FIXED_NOREPLACE, nunca MAP_FIXED: se o endereço já estiver ocupado
    // por outra coisa, é melhor falhar e dizer qual, do que sobrepor
    // silenciosamente a memória de outro processo.
    bool map(std::uint64_t base, std::uint64_t size, Prot prot, std::string& error);

    // Muda as permissões de uma parte do mapeamento. Usado depois do patch:
    // o .text é escrito pelo patcher e depois fica só de leitura+execução.
    bool protect(std::uint64_t base, std::uint64_t size, Prot prot, std::string& error);

    // Escreve `size` bytes em `dst`, que tem de estar dentro do mapeamento.
    bool write(std::uint64_t dst, const void* src, std::uint64_t size, std::string& error);

    // Lê `size` bytes de `src`. Com `scratch`, a leitura é feita por um
    // processo filho separado — o mecanismo clássico para ler memória do
    // guest sem que o acesso do host possa</think>faultar.
    bool read(std::uint64_t src, void* dst, std::uint64_t size, std::string& error);

    // Um trap de memória (SIGSEGV/SIGBUS) dentro do espaço do guest tem de ser
    // distinguido de um trap do host. Isto marca os limites da região guest.
    bool owns(std::uint64_t addr) const noexcept;

    // O ponteiro do HOST para um endereço do guest.
    //
    // Existe porque a camada de IPC do Eden precisa de um `u32*` para a
    // message buffer do TLS (`k_server_session.cpp:379`):
    //
    //     msg_ptr = GetCurrentMemory(kernel).GetPointer<u32>(thread.GetTlsAddress());
    //
    // E porque isso é verdade no nosso caso de forma particular: a reserva é
    // um `mmap` de gama fixa, feito pelo próprio motor, e o endereço do guest
    // **é** o endereço do host. Não há tradução nem page table — a reserva é
    // plana e única.
    //
    // Devolve `nullptr` fora da reserva. Um ponteiro meio válido seria pior
    // que nenhum: quem o usasse sem verificar escreveria no host.
    //
    // **Isto é uma optimização, não uma correcção.** Quem chama tem de tratar
    // o `nullptr`, e o Eden trata — é a diferença entre `GetPointer` e
    // `GetPointerSilent`.
    std::uint8_t* host_pointer(std::uint64_t addr, std::uint64_t size) const noexcept;

    void unmap() noexcept;

    std::uint64_t base()  const noexcept { return base_; }
    std::uint64_t size()  const noexcept { return size_; }
    bool          mapped()const noexcept { return base_ != 0; }

    // Contadores de acesso, para a telemetria. Não custam nada no caminho
    // quente e respondem "o guest está a tocar mais em código ou em heap",
    // que é a pergunta que a telemetria existe para fazer.
    std::uint64_t exec_reads()  const noexcept { return exec_reads_; }
    std::uint64_t exec_writes() const noexcept { return exec_writes_; }
    void reset_counters() noexcept { exec_reads_ = exec_writes_ = 0; }

    // Salto para um endereço guest. É o que `run()` chama; fica aqui para que
    // a verificação de que o endereço pertence ao espaço do guest aconteça num
    // sítio só.
    //
    // Não volta: o código do guest ou chama ExitProcess, ou leva um fault, e
    // ambos chegam ao host pelo mecanismo de recuperação do motor.
    [[noreturn]] static void jump(std::uint64_t addr, std::uint64_t sp);

private:
    std::uint64_t base_ = 0;
    std::uint64_t size_ = 0;
    std::uint64_t exec_reads_  = 0;
    std::uint64_t exec_writes_ = 0;
};

// Alinhamento exigido pela AAPCS64. Um sp desalinhado custa um Alignment
// Fault no primeiro push — e a mensagem não diz nada sobre a causa.
inline constexpr std::uint64_t kStackAlign = 16;

} // namespace ia