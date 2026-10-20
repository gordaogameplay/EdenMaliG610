// A page table do guest, com o nome que o Eden lhe dá.
//
// Ver `vendor/eden/core/hle/kernel/k_process_bridge.h` para o resto da ponte,
// e `memory_bridge.h` para a memória que esta classe devolve.
//
// ## O que substitui
//
// `core/hle/kernel/k_process_page_table.h`, `k_page_table_base.h` e os enums
// de `k_memory_block.h` do Eden.
//
// ## Porquê uma página que não existe
//
// A MEDIDA que libertou isto: neste motor **o endereço do guest é o endereço do
// host**. A reserva é um `mmap` de gama fixa, feito pelo motor, com uma única
// protecção. Não há tradução, não há tabelas, não há páginas de 4 KiB a
// gerir, não há faults a tratar.
//
// Uma page table com um único mapeamento que cobre a reserva inteira é
// **literalmente verdadeira**. Não é uma aproximação: `mmap` de gama fixa em
// Linux mapeia exactamente o que se pede, e nada mais.
//
// ## O que isso transforma
//
// Cada `CopyMemoryFrom*` passa a ser um `memcpy`, e cada `CleanupForIpc*`
// passa a ser um no-op:
//
//     CopyMemoryFromUserToLinear(dst, size, ..., src)
//         -> memcpy(dst, src, size)     // "linear" e "user" sao o mesmo
//
// E `GetMemory()` devolve a `Core::Memory::Memory` da ponte — a mesma que a
// camada de IPC já usava.
//
// ## O que se perde, e é pouco
//
// As flags: `CanUseIpc`, `CanReprotect`, `Code`, e as mais vinte. Nenhuma se
// aplica a um mapeamento único sem protecção. O `SetupForIpc` e os dois
// `CleanupForIpc*` não fazem nada **porque não há nada a preparar nem a
// desfazer** — e a alternativa honesta seria inventar permissões que não
// existem.
//
// ## Onde isto deixaria de ser verdade
//
// Se a reserva deixar de ser única. Um heap separado, uma zona de device
// memory, ou um mapeamento com permissões diferentes por páginas fariam cada
// um destes métodos deixar de ser um `memcpy`.
//
// É o que o `KProcess` do Eden faz, e é o que o nosso motor não faz: um
// processo guest, uma reserva.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "vendor/eden/common/common_types.h"
#include "vendor/eden/common/typed_address.h"
#include "vendor/eden/core/hle/kernel/k_typed_address.h"   // KProcessAddress
// `svc_types.h` traz `Kernel::Svc::*`, de onde os enums copiados de
// `k_memory_block.h` vao buscar os seus valores (por exemplo
// `Svc::PermissionLocked`). Sem ele, o enum compila com o valor errado.
#include "vendor/eden/core/hle/kernel/svc_types.h"
// `Result` completo: os metodos desta classe **devolvem** `Result`, e uma
// `union Result;` declarada a frente nao serve para um valor de retorno.
#include "vendor/eden/core/hle/kernel/svc_results.h"   // ResultInvalidAddress
#include "vendor/eden/core/hle/result.h"
#include "vendor/eden/core/memory_bridge.h"

namespace Kernel {

// PONTE-ENUMS-AQUI
// (o bloco abaixo e' escrito por tools/copiar_enums_memoria.py)

// O destino e a origem de uma cópia. No Eden é uma referência à page table do
// outro processo; aqui é o mesmo objecto, porque há um processo só.
// ── os enums, copiados do Eden ──────────────────────────────────────
//
// De `core/hle/kernel/k_memory_block.h`, **inteiros e com os valores tal e
// qual**. Este bloco foi escrito por `tools/copiar_enums_memoria.py` e é
// canónico: o build não o regenera.
//
// Ver o `docstring` do script para o que aconteceu quando o build o gerava.
//
// Tudo o resto daquele ficheiro (o gestor de blocos, a árvore vermelha-
// preta, o alinhamento) fica de fora: não é preciso para copiar memória, e é
// o que arrastaria o emulador.

enum class KMemoryState : u32 {
    None = 0,
    Mask = 0xFF,
    All = ~None,

    FlagCanReprotect = (1 << 8),
    FlagCanDebug = (1 << 9),
    FlagCanUseIpc = (1 << 10),
    FlagCanUseNonDeviceIpc = (1 << 11),
    FlagCanUseNonSecureIpc = (1 << 12),
    FlagMapped = (1 << 13),
    FlagCode = (1 << 14),
    FlagCanAlias = (1 << 15),
    FlagCanCodeAlias = (1 << 16),
    FlagCanTransfer = (1 << 17),
    FlagCanQueryPhysical = (1 << 18),
    FlagCanDeviceMap = (1 << 19),
    FlagCanAlignedDeviceMap = (1 << 20),
    FlagCanIpcUserBuffer = (1 << 21),
    FlagReferenceCounted = (1 << 22),
    FlagCanMapProcess = (1 << 23),
    FlagCanChangeAttribute = (1 << 24),
    FlagCanCodeMemory = (1 << 25),
    FlagLinearMapped = (1 << 26),
    FlagCanPermissionLock = (1 << 27),

    FlagsData = FlagCanReprotect | FlagCanUseIpc | FlagCanUseNonDeviceIpc | FlagCanUseNonSecureIpc |
                FlagMapped | FlagCanAlias | FlagCanTransfer | FlagCanQueryPhysical |
                FlagCanDeviceMap | FlagCanAlignedDeviceMap | FlagCanIpcUserBuffer |
                FlagReferenceCounted | FlagCanChangeAttribute | FlagLinearMapped,

    FlagsCode = FlagCanDebug | FlagCanUseIpc | FlagCanUseNonDeviceIpc | FlagCanUseNonSecureIpc |
                FlagMapped | FlagCode | FlagCanQueryPhysical | FlagCanDeviceMap |
                FlagCanAlignedDeviceMap | FlagReferenceCounted | FlagLinearMapped,

    FlagsMisc = FlagMapped | FlagReferenceCounted | FlagCanQueryPhysical | FlagCanDeviceMap |
                FlagLinearMapped,

    Free = static_cast<u32>(Svc::MemoryState::Free),

    IoMemory = static_cast<u32>(Svc::MemoryState::Io) | FlagMapped | FlagCanDeviceMap |
               FlagCanAlignedDeviceMap,
    IoRegister =
        static_cast<u32>(Svc::MemoryState::Io) | FlagCanDeviceMap | FlagCanAlignedDeviceMap,

    Static = static_cast<u32>(Svc::MemoryState::Static) | FlagMapped | FlagCanQueryPhysical,
    Code = static_cast<u32>(Svc::MemoryState::Code) | FlagsCode | FlagCanMapProcess,
    CodeData = static_cast<u32>(Svc::MemoryState::CodeData) | FlagsData | FlagCanMapProcess |
               FlagCanCodeMemory | FlagCanPermissionLock,
    Normal = static_cast<u32>(Svc::MemoryState::Normal) | FlagsData | FlagCanCodeMemory,
    Shared = static_cast<u32>(Svc::MemoryState::Shared) | FlagMapped | FlagReferenceCounted |
             FlagLinearMapped,

    // Alias was removed after 1.0.0.

    AliasCode = static_cast<u32>(Svc::MemoryState::AliasCode) | FlagsCode | FlagCanMapProcess |
                FlagCanCodeAlias,
    AliasCodeData = static_cast<u32>(Svc::MemoryState::AliasCodeData) | FlagsData |
                    FlagCanMapProcess | FlagCanCodeAlias | FlagCanCodeMemory |
                    FlagCanPermissionLock,

    Ipc = static_cast<u32>(Svc::MemoryState::Ipc) | FlagsMisc | FlagCanAlignedDeviceMap |
          FlagCanUseIpc | FlagCanUseNonSecureIpc | FlagCanUseNonDeviceIpc,

    Stack = static_cast<u32>(Svc::MemoryState::Stack) | FlagsMisc | FlagCanAlignedDeviceMap |
            FlagCanUseIpc | FlagCanUseNonSecureIpc | FlagCanUseNonDeviceIpc,

    ThreadLocal = static_cast<u32>(Svc::MemoryState::ThreadLocal) | FlagLinearMapped,

    Transferred = static_cast<u32>(Svc::MemoryState::Transferred) | FlagsMisc |
                  FlagCanAlignedDeviceMap | FlagCanChangeAttribute | FlagCanUseIpc |
                  FlagCanUseNonSecureIpc | FlagCanUseNonDeviceIpc,

    SharedTransferred = static_cast<u32>(Svc::MemoryState::SharedTransferred) | FlagsMisc |
                        FlagCanAlignedDeviceMap | FlagCanUseNonSecureIpc | FlagCanUseNonDeviceIpc,

    SharedCode = static_cast<u32>(Svc::MemoryState::SharedCode) | FlagMapped |
                 FlagReferenceCounted | FlagLinearMapped | FlagCanUseNonSecureIpc |
                 FlagCanUseNonDeviceIpc,

    Inaccessible = static_cast<u32>(Svc::MemoryState::Inaccessible),

    NonSecureIpc = static_cast<u32>(Svc::MemoryState::NonSecureIpc) | FlagsMisc |
                   FlagCanAlignedDeviceMap | FlagCanUseNonSecureIpc | FlagCanUseNonDeviceIpc,

    NonDeviceIpc =
        static_cast<u32>(Svc::MemoryState::NonDeviceIpc) | FlagsMisc | FlagCanUseNonDeviceIpc,

    Kernel = static_cast<u32>(Svc::MemoryState::Kernel),

    GeneratedCode = static_cast<u32>(Svc::MemoryState::GeneratedCode) | FlagMapped |
                    FlagReferenceCounted | FlagCanDebug | FlagLinearMapped,
    CodeOut = static_cast<u32>(Svc::MemoryState::CodeOut) | FlagMapped | FlagReferenceCounted |
              FlagLinearMapped,

    Coverage = static_cast<u32>(Svc::MemoryState::Coverage) | FlagMapped,

    Insecure = static_cast<u32>(Svc::MemoryState::Insecure) | FlagMapped | FlagReferenceCounted |
               FlagLinearMapped | FlagCanChangeAttribute | FlagCanDeviceMap |
               FlagCanAlignedDeviceMap | FlagCanQueryPhysical | FlagCanUseNonSecureIpc |
               FlagCanUseNonDeviceIpc,
};
DECLARE_ENUM_FLAG_OPERATORS(KMemoryState);

enum class KMemoryPermission : u8 {
    None = 0,
    All = static_cast<u8>(~None),

    KernelShift = 3,

    KernelRead = static_cast<u8>(Svc::MemoryPermission::Read) << KernelShift,
    KernelWrite = static_cast<u8>(Svc::MemoryPermission::Write) << KernelShift,
    KernelExecute = static_cast<u8>(Svc::MemoryPermission::Execute) << KernelShift,

    NotMapped = (1 << (2 * KernelShift)),

    KernelReadWrite = KernelRead | KernelWrite,
    KernelReadExecute = KernelRead | KernelExecute,

    UserRead = static_cast<u8>(Svc::MemoryPermission::Read) | KernelRead,
    UserWrite = static_cast<u8>(Svc::MemoryPermission::Write) | KernelWrite,
    UserExecute = static_cast<u8>(Svc::MemoryPermission::Execute),

    UserReadWrite = UserRead | UserWrite,
    UserReadExecute = UserRead | UserExecute,

    UserMask = static_cast<u8>(Svc::MemoryPermission::Read | Svc::MemoryPermission::Write |
                               Svc::MemoryPermission::Execute),

    IpcLockChangeMask = NotMapped | UserReadWrite,
};
DECLARE_ENUM_FLAG_OPERATORS(KMemoryPermission);

enum class KMemoryAttribute : u8 {
    None = 0x00,
    All = 0xFF,
    UserMask = All,

    Locked = static_cast<u8>(Svc::MemoryAttribute::Locked),
    IpcLocked = static_cast<u8>(Svc::MemoryAttribute::IpcLocked),
    DeviceShared = static_cast<u8>(Svc::MemoryAttribute::DeviceShared),
    Uncached = static_cast<u8>(Svc::MemoryAttribute::Uncached),
    PermissionLocked = static_cast<u8>(Svc::MemoryAttribute::PermissionLocked),

    SetMask = Uncached | PermissionLocked,
};
DECLARE_ENUM_FLAG_OPERATORS(KMemoryAttribute);

class KPageTableBase {
public:
    explicit KPageTableBase(Core::Memory::Memory& memoria) : memoria_(memoria) {}

    Core::Memory::Memory& GetMemory() noexcept { return memoria_; }
    const Core::Memory::Memory& GetMemory() const noexcept { return memoria_; }

    // ── as cópias ─────────────────────────────────────────────────────────
    //
    // Todas `memcpy`. Ver a nota no topo do ficheiro: "linear" e "user" são o
    // mesmo endereço, e "kernel" é um ponteiro do host para memória que o
    // Eden copiaria para uma página nova.
    //
    // As `*_state_mask`, `*_test_perm` e `*_attr` que o Eden usa para
    // verificar a página antes de copiar ficam sem uso, e ficam **sem
    // verificar**: um único mapeamento não tem estado para verificar.
    //
    // A consequência: um acesso a uma página que não existe devolve
    // `ResultInvalidAddress` em vez de `ResultInvalidMemoryState`. As duas
    // coisas são visíveis e distintas, e a segunda é a que o Eden daria.

    Result CopyMemoryFromLinearToUser(KProcessAddress dst_addr, size_t size,
                                      KProcessAddress src_addr, KMemoryState src_state_mask,
                                      KMemoryState src_state, KMemoryPermission src_test_perm,
                                      KMemoryAttribute src_attr_mask, KMemoryAttribute src_attr) {
        return copiar(src_addr, dst_addr, size);
    }

    Result CopyMemoryFromLinearToKernel(void* buffer, size_t size, KProcessAddress src_addr,
                                        KMemoryState src_state_mask, KMemoryState src_state,
                                        KMemoryPermission src_test_perm,
                                        KMemoryAttribute src_attr_mask,
                                        KMemoryAttribute src_attr) {
        return copiar(src_addr, de_kernel(buffer), size);
    }

    Result CopyMemoryFromUserToLinear(KProcessAddress dst_addr, size_t size,
                                      KMemoryState dst_state_mask, KMemoryState dst_state,
                                      KMemoryPermission dst_test_perm,
                                      KMemoryAttribute dst_attr_mask, KMemoryAttribute dst_attr,
                                      KProcessAddress src_addr) {
        return copiar(src_addr, dst_addr, size);
    }

    Result CopyMemoryFromKernelToLinear(KProcessAddress dst_addr, size_t size,
                                        KMemoryState dst_state_mask, KMemoryState dst_state,
                                        KMemoryPermission dst_test_perm,
                                        KMemoryAttribute dst_attr_mask, KMemoryAttribute dst_attr,
                                        void* buffer) {
        return copiar(de_kernel(buffer), dst_addr, size);
    }

    Result CopyMemoryFromHeapToHeap(KPageTableBase& /*dst_page_table*/, KProcessAddress dst_addr,
                                    size_t size, KMemoryState dst_state_mask,
                                    KMemoryState dst_state, KMemoryPermission dst_test_perm,
                                    KMemoryAttribute dst_attr_mask, KMemoryAttribute dst_attr,
                                    KProcessAddress src_addr, KMemoryState src_state_mask,
                                    KMemoryState src_state, KMemoryPermission src_test_perm,
                                    KMemoryAttribute src_attr_mask, KMemoryAttribute src_attr) {
        return copiar(src_addr, dst_addr, size);
    }

    Result CopyMemoryFromHeapToHeapWithoutCheckDestination(
        KPageTableBase& /*dst_page_table*/, KProcessAddress dst_addr, size_t size,
        KMemoryState dst_state_mask, KMemoryState dst_state, KMemoryPermission dst_test_perm,
        KMemoryAttribute dst_attr_mask, KMemoryAttribute dst_attr, KProcessAddress src_addr,
        KMemoryState src_state_mask, KMemoryState src_state, KMemoryPermission src_test_perm,
        KMemoryAttribute src_attr_mask, KMemoryAttribute src_attr) {
        return copiar(src_addr, dst_addr, size);
    }

    // ── o ciclo de vida de um mapeamento ──────────────────────────────────
    //
    // No Eden, `SetupForIpc` mapeia as páginas de um buffer de utilizador para
    // o processo servidor, e os dois `CleanupForIpc*` desfazem isso quando o
    // pedido acaba.
    //
    // Aqui **não há nada a mapear**: a reserva do guest é um único `mmap`, e um
    // buffer de utilizador já está dentro dela. O `SetupForIpc` limita-se a
    // devolver o endereço de origem, que é a operação inteira.
    Result SetupForIpc(KProcessAddress* out_dst_addr, size_t size, KProcessAddress src_addr,
                       KPageTableBase& /*src_page_table*/, KMemoryPermission /*test_perm*/,
                       KMemoryState /*dst_state*/, bool /*send*/) {
        if (out_dst_addr != nullptr)
            *out_dst_addr = src_addr;
        return ResultSuccess;
    }

    Result CleanupForIpcServer(KProcessAddress /*address*/, size_t /*size*/,
                              KMemoryState /*dst_state*/) {
        return ResultSuccess;
    }
    Result CleanupForIpcClient(KProcessAddress /*address*/, size_t /*size*/,
                               KMemoryState /*dst_state*/) {
        return ResultSuccess;
    }

    // ── o bloqueio de um buffer de utilizador ────────────────────────────
    //
    // No Eden, `LockForIpcUserBuffer` localiza as páginas de um buffer do
    // guest e devolve o endereço físico, e o `Unlock` desfaz. O par existe
    // porque a cópia para o servidor tem de ler de páginas que o processo
    // cliente não pode mudar por baixo.
    //
    // Aqui **não há nada a bloquear**: uma reserva só, sem permissões, sem
    // verificação de concorrência. O `Lock` reporta o endereço de origem — que
    // é a informação que o Eden lá põe, embora num espaço diferente — e o
    // `Unlock` não faz nada porque não havia estado.
    Result LockForIpcUserBuffer(KPhysicalAddress* out, KProcessAddress address, size_t size) {
        if (out != nullptr)
            *out = KPhysicalAddress{address.GetValue()};
        return ResultSuccess;
    }
    Result UnlockForIpcUserBuffer(KProcessAddress /*address*/, size_t /*size*/) {
        return ResultSuccess;
    }

    // Um mapeamento único que cobre a reserva: a pergunta é se o bloco pedido
    // cabe inteiro dentro dela.
    bool Contains(KProcessAddress addr, size_t size) const { return dentro(addr, size); }

private:
    static KProcessAddress de_kernel(void* buffer) {
        return KProcessAddress{reinterpret_cast<std::uintptr_t>(buffer)};
    }

    Result copiar(KProcessAddress de, KProcessAddress para, size_t size) {
        if (size == 0)
            return ResultSuccess;
        if (!dentro(de, size) || !dentro(para, size))
            return ResultInvalidAddress;
        std::memcpy(reinterpret_cast<void*>(para.GetValue()),
                    reinterpret_cast<const void*>(de.GetValue()), size);
        return ResultSuccess;
    }

    // Escrita sem overflow, pelo mesmo motivo que em `GuestMemory::owns`: um
    // `size` grande somado a um endereço perto do fim daria "dentro".
    bool dentro(KProcessAddress addr, size_t size) const {
        const std::uint64_t inicio = addr.GetValue();
        const std::uint64_t base = memoria_.base();
        const std::uint64_t total = memoria_.size_bytes();
        if (total == 0 || inicio < base)
            return false;
        const std::uint64_t desvio = inicio - base;
        if (size > total || desvio > total - size)
            return false;
        return memoria_.host_pointer(inicio, size) != nullptr;
    }

    Core::Memory::Memory& memoria_;
};

// A page table do processo. Uma por processo, e há um.
using KProcessPageTable = KPageTableBase;

} // namespace Kernel