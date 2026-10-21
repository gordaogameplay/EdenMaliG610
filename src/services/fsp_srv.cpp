// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
// O `fsp-srv` — o servidor de ficheiros — **nosso**.
//
// ## O que é
//
// O `fsp-srv` é o serviço de ficheiros do Switch. O SDK do jogo pede-o ao
// `sm:` durante o arranque, e é este serviço que dá ao jogo o sistema de
// ficheiros do seu programa (o "romfs" empacotado no NCA) e o directório
// dos dados guardados.
//
// ## Porque este ficheiro existe
//
// **Medido** (`boot_p6f.log`): o guest pede `GetService("fsp-srv")` e,
// como o serviço não está registado, o `sm:` **defere** o pedido. O
// diferimento, no motor nativo, é fatal:
//
//   1. o `sm:` responde com **0 palavras** (o `GetServiceCmif` do Eden
//      devolve sem tocar no buffer quando o pedido foi diferido);
//   2. o `SendSyncRequest` do guest **retoma como se tivesse havido
//      resposta**;
//   3. o reader CMIF do guest recebe zero palavras onde espera a
//      Result + handle, e aborta — `SetTerminateResult: modulo=10
//      desc=212` (o `CMIF`).
//
// No Eden isto é seguro porque o guest **emulado** fica bloqueado no
// kernel à espera da resposta; no nosso motor o guest é **nativo** e
// continua. A diferença é o que matou o jogo.
//
// ## A forma é do Eden, à letra
//
// Os **IDs dos comandos**, os **nomes**, e o conjunto de comandos que o
// Eden implementa e o que deixa por implementar — tudo copiado de
// `core/hle/service/filesystem/fsp/fsp_srv.cpp`. É o contrato com o
// guest: o ID errado e o guest lê o campo errado, e um comando que o
// Eden implementa e nós não, é um comando que o guest fica à espera.
//
// ## O conteúdo é o mínimo, e a política é explícita
//
// O Eden resolve isto contra o `romfs` do processo e o `NAND`/`SD`
// reais. Nós temos um **ExeFS** (o jogo foi desencriptado para
// ficheiros) e nenhum sistema de ficheiros por trás. A política é:
//
//   - o que **abre** algo (um sistema de ficheiros, um armazenamento, um
//     leitor de saves) devolve uma interface **real e vazia** — o guest
//     continua, e o log diz o que não há;
//   - o que **procura** algo (abrir um ficheiro, o tipo de uma entrada)
//     devolve o erro do Eden (`FS`/2, `PathNotFound`) — que é a resposta
//     certa para "não existe";
//   - o que **escreve** (criar, gravar) devolve sucesso e **não faz
//     nada** — porque um erro aqui mata o jogo, e o que o guest quer
//     escrever não existe em nenhum sítio.
//
// ## O que NÃO está aqui
//
// O `filesystem.cpp` do Eden tem 26 000 linhas e arrasta o `VfsFilesystem`,
// o `BISFactory`, o `SaveDataController` e o `RomfsController` — ou seja,
// o sistema de ficheiros inteiro. Trazer isso é trazer o emulador.
//
// SPDX-FileCopyrightText: 2018 Yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso
#include "vendor/eden/core/hle/kernel/k_event.h"
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/service.h"

namespace g610::services {

namespace {

using Service::HLERequestContext;

using IPC::RequestParser;
using IPC::ResponseBuilder;

// ─────────────────────────────────────────────────────────────────────────────
// Utilitários
// ─────────────────────────────────────────────────────────────────────────────

// O erro do Eden para "o caminho não existe": módulo `FS` (2),
// descrição 2 (`PathNotFound`).
Result PathNotFound() {
    return Result{ErrorModule::FS, 2};
}

// O caminho de um ficheiro vem num **ponteiro** (o `FileSys::Sf::Path` do
// Eden: `char str[65]`), passado como buffer HIPC. Lê-se o buffer 0 e
// fica-se com o que é legível — o valor só interessa para o log.
std::string LerCaminho(HLERequestContext& ctx) {
    const auto bytes = ctx.ReadBufferCopy(0);
    std::string s;
    for (const auto b : bytes) {
        if (b == 0)
            break;
        if (b < 0x20 || b > 0x7E)
            break;
        s.push_back(static_cast<char>(b));
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// O `IFile` — um ficheiro aberto.
//
// Tabela do Eden (`fs_i_file.cpp`), à letra.
// ─────────────────────────────────────────────────────────────────────────────
class IFile final : public Service::ServiceFramework<IFile> {
public:
    explicit IFile(Core::System& system) : ServiceFramework{system, "IFile"} {
        static const FunctionInfo functions[] = {
            {0, &IFile::Read, "Read"},
            {1, &IFile::Write, "Write"},
            {2, &IFile::Flush, "Flush"},
            {3, &IFile::SetSize, "SetSize"},
            {4, &IFile::GetSize, "GetSize"},
            {5, nullptr, "OperateRange"},
            {6, nullptr, "OperateRangeWithBuffer"},
        };
        RegisterHandlers(functions);
    }

private:
    // 0: ler. A resposta do Eden é `Result` + o número de bytes lidos
    // (`Out<s64>`). Não há ficheiro nenhum, logo zero bytes.
    void Read(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFile::Read: sem conteúdo");
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::int64_t>(0);
    }

    // 1: escrever. Não há sítio onde escrever; o `Result` é sucesso
    // (a política está no cabeçalho do ficheiro) e o log diz a verdade.
    void Write(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFile::Write: ignorado (sem armazenamento)");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Flush(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetSize(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFile::SetSize: ignorado (sem armazenamento)");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 4: o tamanho do ficheiro. Zero — o ficheiro não existe.
    void GetSize(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::int64_t>(0);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O `IDirectory` — um directório aberto.
//
// Tabela do Eden (`fs_i_directory.cpp`).
// ─────────────────────────────────────────────────────────────────────────────
class IDirectory final : public Service::ServiceFramework<IDirectory> {
public:
    explicit IDirectory(Core::System& system) : ServiceFramework{system, "IDirectory"} {
        static const FunctionInfo functions[] = {
            {0, &IDirectory::Read, "Read"},
            {1, &IDirectory::GetEntryCount, "GetEntryCount"},
        };
        RegisterHandlers(functions);
    }

private:
    // 0: ler as entradas. `Result` + o número (`Out<s64>`): zero.
    void Read(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IDirectory::Read: directório vazio");
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::int64_t>(0);
    }

    void GetEntryCount(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::int64_t>(0);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O `IFileSystem` — o sistema de ficheiros.
//
// Tabela do Eden (`fs_i_filesystem.cpp`), à letra.
// ─────────────────────────────────────────────────────────────────────────────
class IFileSystem final : public Service::ServiceFramework<IFileSystem> {
public:
    explicit IFileSystem(Core::System& system) : ServiceFramework{system, "IFileSystem"} {
        static const FunctionInfo functions[] = {
            {0, &IFileSystem::CreateFile, "CreateFile"},
            {1, &IFileSystem::DeleteFile, "DeleteFile"},
            {2, &IFileSystem::CreateDirectory, "CreateDirectory"},
            {3, &IFileSystem::DeleteDirectory, "DeleteDirectory"},
            {4, &IFileSystem::DeleteDirectoryRecursively, "DeleteDirectoryRecursively"},
            {5, &IFileSystem::RenameFile, "RenameFile"},
            {6, nullptr, "RenameDirectory"},
            {7, &IFileSystem::GetEntryType, "GetEntryType"},
            {8, &IFileSystem::OpenFile, "OpenFile"},
            {9, &IFileSystem::OpenDirectory, "OpenDirectory"},
            {10, &IFileSystem::Commit, "Commit"},
            {11, &IFileSystem::GetFreeSpaceSize, "GetFreeSpaceSize"},
            {12, &IFileSystem::GetTotalSpaceSize, "GetTotalSpaceSize"},
            {13, &IFileSystem::CleanDirectoryRecursively, "CleanDirectoryRecursively"},
            {14, &IFileSystem::GetFileTimeStampRaw, "GetFileTimeStampRaw"},
            {15, nullptr, "QueryEntry"},
            {16, nullptr, "GetFileSystemAttribute"},
        };
        RegisterHandlers(functions);
    }

private:
    // O `DirectoryEntryType` do Eden: 0 = desconhecido, 1 = ficheiro,
    // 2 = directório. Um sistema de ficheiros vazio não tem entradas.
    static constexpr std::uint32_t kEntryTypeUnknown = 0;

    // 0 a 5: as operações que escrevem. Não há onde escrever: o
    // `Result` é sucesso e o log regista a verdade.
    void CreateFile(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::CreateFile: '{}' (ignorado)", LerCaminho(ctx));
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void DeleteFile(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::DeleteFile: '{}' (ignorado)", LerCaminho(ctx));
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CreateDirectory(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::CreateDirectory: '{}' (ignorado)", LerCaminho(ctx));
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void DeleteDirectory(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::DeleteDirectory: '{}' (ignorado)", LerCaminho(ctx));
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void DeleteDirectoryRecursively(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::DeleteDirectoryRecursively: '{}' (ignorado)",
                  LerCaminho(ctx));
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void RenameFile(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::RenameFile: '{}' (ignorado)", LerCaminho(ctx));
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 7: o tipo de uma entrada. Nada existe, logo "desconhecido" — que
    // é o que o Eden devolve para um caminho que não resolveu.
    void GetEntryType(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::GetEntryType: '{}' -> desconhecido", LerCaminho(ctx));
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<std::uint32_t>(kEntryTypeUnknown);
    }

    // 8: abrir um ficheiro. **Aqui o erro é a resposta certa**: o Eden
    // devolve o erro do backend quando o ficheiro não existe, e o guest
    // sabe tratar esse erro (é o caminho normal de "o ficheiro não está
    // lá").
    void OpenFile(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::OpenFile: '{}' -> não existe", LerCaminho(ctx));
        ResponseBuilder rb{ctx, 2};
        rb.Push(PathNotFound());
    }

    // 9: abrir um directório. Pelo mesmo: não existe.
    void OpenDirectory(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::OpenDirectory: '{}' -> não existe", LerCaminho(ctx));
        ResponseBuilder rb{ctx, 2};
        rb.Push(PathNotFound());
    }

    // 10: confirmar as escritas. Não há escritas.
    void Commit(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::Commit: nada a confirmar");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 11 e 12: o espaço livre e o total. Zero — não há sistema de
    // ficheiros por trás, e **zero é honesto**.
    void GetFreeSpaceSize(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::int64_t>(0);
    }

    void GetTotalSpaceSize(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::int64_t>(0);
    }

    void CleanDirectoryRecursively(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::CleanDirectoryRecursively: '{}' (ignorado)",
                  LerCaminho(ctx));
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 14: a data/hora do ficheiro. O Eden devolve duas structures de
    // 64 bits (criação e modificação); sem ficheiro, zeros.
    void GetFileTimeStampRaw(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IFileSystem::GetFileTimeStampRaw: '{}'", LerCaminho(ctx));
        ResponseBuilder rb{ctx, 6};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(0);
        rb.Push<std::uint64_t>(0);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O `IStorage` — o armazenamento de dados do processo (o "romfs").
//
// Tabela do Eden (`fs_i_storage.cpp`).
// ─────────────────────────────────────────────────────────────────────────────
class IStorage final : public Service::ServiceFramework<IStorage> {
public:
    explicit IStorage(Core::System& system) : ServiceFramework{system, "IStorage"} {
        static const FunctionInfo functions[] = {
            {0, &IStorage::Read, "Read"},
            {1, nullptr, "Write"},
            {2, nullptr, "Flush"},
            {3, nullptr, "SetSize"},
            {4, &IStorage::GetSize, "GetSize"},
            {5, nullptr, "OperateRange"},
        };
        RegisterHandlers(functions);
    }

private:
    // 0: ler do armazenamento. Sem romfs, zero bytes. O `Result` é
    // sucesso — o Eden também não falha aqui, devolve a leitura vazia.
    void Read(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IStorage::Read: vazio (sem romfs)");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 4: o tamanho do armazenamento. Zero.
    void GetSize(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(0);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O `ISaveDataInfoReader` — lê a informação dos saves.
//
// Tabela do Eden (`fs_i_save_data_info_reader.cpp`).
// ─────────────────────────────────────────────────────────────────────────────
class ISaveDataInfoReader final : public Service::ServiceFramework<ISaveDataInfoReader> {
public:
    explicit ISaveDataInfoReader(Core::System& system)
        : ServiceFramework{system, "ISaveDataInfoReader"} {
        static const FunctionInfo functions[] = {
            {0, &ISaveDataInfoReader::ReadSaveDataInfo, "ReadSaveDataInfo"},
        };
        RegisterHandlers(functions);
    }

private:
    // 0: a lista de saves. `Result` + o número (`Out<u64>`): zero.
    void ReadSaveDataInfo(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "ISaveDataInfoReader::ReadSaveDataInfo: nenhum save");
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(0);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O `ISaveDataTransferProhibiter` — impede transferências de saves.
//
// O Eden também não tem comandos aqui: é uma interface com o corpo
// vazio (`save_data_transfer_prohibiter.cpp`). A **forma** é o que
// importa: o guest recebe o handle e não usa.
// ─────────────────────────────────────────────────────────────────────────────
class ISaveDataTransferProhibiter final
    : public Service::ServiceFramework<ISaveDataTransferProhibiter> {
public:
    explicit ISaveDataTransferProhibiter(Core::System& system)
        : ServiceFramework{system, "ISaveDataTransferProhibiter"} {}
};

// ─────────────────────────────────────────────────────────────────────────────
// O `IMultiCommitManager` — confirma vários sistemas de ficheiros de uma
// vez.
//
// Tabela do Eden (`fs_i_multi_commit_manager.cpp`).
// ─────────────────────────────────────────────────────────────────────────────
class IMultiCommitManager final : public Service::ServiceFramework<IMultiCommitManager> {
public:
    explicit IMultiCommitManager(Core::System& system)
        : ServiceFramework{system, "IMultiCommitManager"} {
        static const FunctionInfo functions[] = {
            {1, &IMultiCommitManager::Add, "Add"},
            {2, &IMultiCommitManager::Commit, "Commit"},
        };
        RegisterHandlers(functions);
    }

private:
    void Add(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IMultiCommitManager::Add: nada a confirmar");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Commit(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "IMultiCommitManager::Commit: nada a confirmar");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O `fsp-srv` propriamente dito.
//
// A tabela é a do Eden (`fsp_srv.cpp`), com os IDs, os nomes, e — o que
// importa mais — **o mesmo conjunto de comandos implementados**. Onde o
// Eden põe `nullptr`, também nós: o `ReportUnimplementedFunction` do
// Eden responde com sucesso e o log diz "NAO IMPLEMENTADO", que é a
// lista de trabalho.
// ─────────────────────────────────────────────────────────────────────────────
class FspSrv final : public Service::ServiceFramework<FspSrv> {
public:
    explicit FspSrv(Core::System& system) : ServiceFramework{system, "fsp-srv"} {
        static const FunctionInfo functions[] = {
            {0, nullptr, "OpenFileSystem"},
            {1, &FspSrv::SetCurrentProcess, "SetCurrentProcess"},
            {2, nullptr, "OpenDataFileSystemByCurrentProcess"},
            {7, &FspSrv::OpenFileSystemWithPatch, "OpenFileSystemWithPatch"},
            {8, nullptr, "OpenFileSystemWithId"},
            {9, nullptr, "OpenDataFileSystemByApplicationId"},
            {11, nullptr, "OpenBisFileSystem"},
            {12, nullptr, "OpenBisStorage"},
            {13, nullptr, "InvalidateBisCache"},
            {17, nullptr, "OpenHostFileSystem"},
            {18, &FspSrv::OpenSdCardFileSystem, "OpenSdCardFileSystem"},
            {19, nullptr, "FormatSdCardFileSystem"},
            {21, nullptr, "DeleteSaveDataFileSystem"},
            {22, &FspSrv::CreateSaveDataFileSystem, "CreateSaveDataFileSystem"},
            {23, &FspSrv::CreateSaveDataFileSystemBySystemSaveDataId,
             "CreateSaveDataFileSystemBySystemSaveDataId"},
            {24, nullptr, "RegisterSaveDataFileSystemAtomicDeletion"},
            {25, nullptr, "DeleteSaveDataFileSystemBySaveDataSpaceId"},
            {26, nullptr, "FormatSdCardDryRun"},
            {27, &FspSrv::IsExFatSupported, "IsExFatSupported"},
            {28, nullptr, "DeleteSaveDataFileSystemBySaveDataAttribute"},
            {30, nullptr, "OpenGameCardStorage"},
            {31, nullptr, "OpenGameCardFileSystem"},
            {32, &FspSrv::ExtendSaveDataFileSystem, "ExtendSaveDataFileSystem"},
            {33, nullptr, "DeleteCacheStorage"},
            {34, nullptr, "GetCacheStorageSize"},
            {35, nullptr, "CreateSaveDataFileSystemByHashSalt"},
            {36, nullptr, "OpenHostFileSystemWithOption"},
            {37, &FspSrv::CreateSaveDataFileSystemWithCreationInfo2,
             "CreateSaveDataFileSystemWithCreationInfo2"},
            {51, &FspSrv::OpenSaveDataFileSystem, "OpenSaveDataFileSystem"},
            {52, &FspSrv::OpenSaveDataFileSystemBySystemSaveDataId,
             "OpenSaveDataFileSystemBySystemSaveDataId"},
            {53, &FspSrv::OpenReadOnlySaveDataFileSystem, "OpenReadOnlySaveDataFileSystem"},
            {57, &FspSrv::ReadSaveDataFileSystemExtraDataBySaveDataSpaceId,
             "ReadSaveDataFileSystemExtraDataBySaveDataSpaceId"},
            {58, &FspSrv::ReadSaveDataFileSystemExtraData, "ReadSaveDataFileSystemExtraData"},
            {59, &FspSrv::WriteSaveDataFileSystemExtraData, "WriteSaveDataFileSystemExtraData"},
            {60, nullptr, "OpenSaveDataInfoReader"},
            {61, &FspSrv::OpenSaveDataInfoReaderBySaveDataSpaceId,
             "OpenSaveDataInfoReaderBySaveDataSpaceId"},
            {62, &FspSrv::OpenSaveDataInfoReaderOnlyCacheStorage,
             "OpenSaveDataInfoReaderOnlyCacheStorage"},
            {64, nullptr, "OpenSaveDataInternalStorageFileSystem"},
            {65, nullptr, "UpdateSaveDataMacForDebug"},
            {66, nullptr, "WriteSaveDataFileSystemExtraData2"},
            {67, &FspSrv::FindSaveDataWithFilter, "FindSaveDataWithFilter"},
            {68, nullptr, "OpenSaveDataInfoReaderBySaveDataFilter"},
            {69, &FspSrv::ReadSaveDataFileSystemExtraDataBySaveDataAttribute,
             "ReadSaveDataFileSystemExtraDataBySaveDataAttribute"},
            {70, &FspSrv::WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute,
             "WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute"},
            {71, &FspSrv::ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute,
             "ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute"},
            {80, nullptr, "OpenSaveDataMetaFile"},
            {81, nullptr, "OpenSaveDataTransferManager"},
            {82, nullptr, "OpenSaveDataTransferManagerVersion2"},
            {83, &FspSrv::OpenSaveDataTransferProhibiter, "OpenSaveDataTransferProhibiter"},
            {84, nullptr, "ListApplicationAccessibleSaveDataOwnerId"},
            {85, nullptr, "OpenSaveDataTransferManagerForSaveDataRepair"},
            {86, nullptr, "OpenSaveDataMover"},
            {87, nullptr, "OpenSaveDataTransferManagerForRepair"},
            {100, nullptr, "OpenImageDirectoryFileSystem"},
            {101, nullptr, "OpenBaseFileSystem"},
            {102, nullptr, "FormatBaseFileSystem"},
            {110, nullptr, "OpenContentStorageFileSystem"},
            {120, nullptr, "OpenCloudBackupWorkStorageFileSystem"},
            {130, nullptr, "OpenCustomStorageFileSystem"},
            {200, &FspSrv::OpenDataStorageByCurrentProcess, "OpenDataStorageByCurrentProcess"},
            {201, nullptr, "OpenDataStorageByProgramId"},
            {202, nullptr, "OpenDataStorageByDataId"},
            {203, nullptr, "OpenPatchDataStorageByCurrentProcess"},
            {204, nullptr, "OpenDataFileSystemByProgramIndex"},
            {205, nullptr, "OpenDataStorageWithProgramIndex"},
            {206, nullptr, "OpenDataStorageByPath"},
            {400, nullptr, "OpenDeviceOperator"},
            {500, nullptr, "OpenSdCardDetectionEventNotifier"},
            {501, nullptr, "OpenGameCardDetectionEventNotifier"},
            {510, nullptr, "OpenSystemDataUpdateEventNotifier"},
            {511, nullptr, "NotifySystemDataUpdateEvent"},
            {520, nullptr, "SimulateGameCardDetectionEvent"},
            {600, nullptr, "SetCurrentPosixTime"},
            {601, nullptr, "QuerySaveDataTotalSize"},
            {602, nullptr, "VerifySaveDataFileSystem"},
            {603, nullptr, "CorruptSaveDataFileSystem"},
            {604, nullptr, "CreatePaddingFile"},
            {605, nullptr, "DeleteAllPaddingFiles"},
            {606, nullptr, "GetRightsId"},
            {607, nullptr, "RegisterExternalKey"},
            {608, nullptr, "UnregisterAllExternalKey"},
            {609, nullptr, "GetRightsIdByPath"},
            {610, nullptr, "GetRightsIdAndKeyGenerationByPath"},
            {611, nullptr, "SetCurrentPosixTimeWithTimeDifference"},
            {612, nullptr, "GetFreeSpaceSizeForSaveData"},
            {613, nullptr, "VerifySaveDataFileSystemBySaveDataSpaceId"},
            {614, nullptr, "CorruptSaveDataFileSystemBySaveDataSpaceId"},
            {615, nullptr, "QuerySaveDataInternalStorageTotalSize"},
            {616, nullptr, "GetSaveDataCommitId"},
            {617, nullptr, "UnregisterExternalKey"},
            {620, nullptr, "SetSdCardEncryptionSeed"},
            {630, nullptr, "SetSdCardAccessibility"},
            {631, nullptr, "IsSdCardAccessible"},
            {640, nullptr, "IsSignedSystemPartitionOnSdCardValid"},
            {700, nullptr, "OpenAccessFailureResolver"},
            {701, nullptr, "GetAccessFailureDetectionEvent"},
            {702, nullptr, "IsAccessFailureDetected"},
            {710, nullptr, "ResolveAccessFailure"},
            {720, nullptr, "AbandonAccessFailure"},
            {800, nullptr, "GetAndClearFileSystemProxyErrorInfo"},
            {810, nullptr, "RegisterProgramIndexMapInfo"},
            {1000, nullptr, "SetBisRootForHost"},
            {1001, nullptr, "SetSaveDataSize"},
            {1002, nullptr, "SetSaveDataRootPath"},
            {1003, &FspSrv::DisableAutoSaveDataCreation, "DisableAutoSaveDataCreation"},
            {1004, &FspSrv::SetGlobalAccessLogMode, "SetGlobalAccessLogMode"},
            {1005, &FspSrv::GetGlobalAccessLogMode, "GetGlobalAccessLogMode"},
            {1006, &FspSrv::OutputAccessLogToSdCard, "OutputAccessLogToSdCard"},
            {1007, nullptr, "RegisterUpdatePartition"},
            {1008, nullptr, "OpenRegisteredUpdatePartition"},
            {1009, nullptr, "GetAndClearMemoryReportInfo"},
            {1010, nullptr, "SetDataStorageRedirectTarget"},
            {1011, &FspSrv::GetProgramIndexForAccessLog, "GetProgramIndexForAccessLog"},
            {1012, nullptr, "GetFsStackUsage"},
            {1013, nullptr, "UnsetSaveDataRootPath"},
            {1014, nullptr, "OutputMultiProgramTagAccessLog"},
            {1016, &FspSrv::FlushAccessLogOnSdCard, "FlushAccessLogOnSdCard"},
            {1017, &FspSrv::OutputApplicationInfoAccessLog, "OutputApplicationInfoAccessLog"},
            {1018, &FspSrv::SetDebugOption, "SetDebugOption"},
            {1019, nullptr, "UnsetDebugOption"},
            {1100, nullptr, "OverrideSaveDataTransferTokenSignVerificationKey"},
            {1110, nullptr, "CorruptSaveDataFileSystemBySaveDataSpaceId2"},
            {1200, &FspSrv::OpenMultiCommitManager, "OpenMultiCommitManager"},
            {1300, nullptr, "OpenBisWiper"},
        };
        RegisterHandlers(functions);
    }

private:
    // 1: o jogo diz qual é o seu PID. O Eden usa-o para abrir o `romfs`
    // certo; nós só o registamos — e o log mostra-o, que é a
    // informação que interessa.
    void SetCurrentProcess(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const std::uint64_t pid = rp.PopRaw<std::uint64_t>();
        LOG_DEBUG(Service_FS, "fsp-srv::SetCurrentProcess: pid=0x{:016X}", pid);
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 7: o sistema de ficheiros "com patch". O Eden abre o romfs
    // manual; nós abrimos um sistema de ficheiros **vazio** — que é a
    // resposta honesta para um ExeFS sem romfs, e deixa o guest
    // seguir em vez de o matar.
    void OpenFileSystemWithPatch(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenFileSystemWithPatch: vazio (sem romfs)");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IFileSystem>(ctx, std::make_shared<IFileSystem>(system));
    }

    // 18: o sistema de ficheiros do cartão SD. Também vazio — sem o
    // `VfsFilesystem` do Eden, não há cartão.
    void OpenSdCardFileSystem(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenSdCardFileSystem: vazio (sem VfsFilesystem)");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IFileSystem>(ctx, std::make_shared<IFileSystem>(system));
    }

    // 22, 23 e 37: criar o directório de dados guardados. O Eden grava
    // no NAND/SD reais. Aqui não há sítio: a resposta é sucesso (um erro
    // matava o jogo) e o log regista que nada foi criado.
    void CreateSaveDataFileSystem(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::CreateSaveDataFileSystem: nada criado (sem sítio)");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IFileSystem>(ctx, std::make_shared<IFileSystem>(system));
    }

    void CreateSaveDataFileSystemBySystemSaveDataId(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS,
                  "fsp-srv::CreateSaveDataFileSystemBySystemSaveDataId: nada criado");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IFileSystem>(ctx, std::make_shared<IFileSystem>(system));
    }

    void CreateSaveDataFileSystemWithCreationInfo2(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::CreateSaveDataFileSystemWithCreationInfo2: nada criado");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IFileSystem>(ctx, std::make_shared<IFileSystem>(system));
    }

    // 27: o cartão é exFAT? O Eden diz que sim.
    void IsExFatSupported(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<std::uint32_t>(1);
    }

    // 32: estender o directório de dados. Não há directório.
    void ExtendSaveDataFileSystem(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::ExtendSaveDataFileSystem: nada estendido");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IFileSystem>(ctx, std::make_shared<IFileSystem>(system));
    }

    // 51, 52 e 53: abrir os dados guardados. O Eden abre o directório
    // real; nós abrimos um sistema de ficheiros **vazio** — o guest
    // continua, e o log diz o que não há.
    void OpenSaveDataFileSystem(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenSaveDataFileSystem: vazio (sem directório de dados)");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IFileSystem>(ctx, std::make_shared<IFileSystem>(system));
    }

    void OpenSaveDataFileSystemBySystemSaveDataId(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenSaveDataFileSystemBySystemSaveDataId: vazio");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IFileSystem>(ctx, std::make_shared<IFileSystem>(system));
    }

    void OpenReadOnlySaveDataFileSystem(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenReadOnlySaveDataFileSystem: vazio");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IFileSystem>(ctx, std::make_shared<IFileSystem>(system));
    }

    // 57 e 58: os "dados extra" do directório de saves (um bloco de
    // bytes opaco). Não há directório, logo o bloco está vazio — e o
    // Eden também devolve sucesso com o buffer vazio.
    void ReadSaveDataFileSystemExtraDataBySaveDataSpaceId(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::ReadSaveDataFileSystemExtraDataBySaveDataSpaceId: vazio");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ReadSaveDataFileSystemExtraData(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::ReadSaveDataFileSystemExtraData: vazio");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 59: escrever os dados extra. Ignorado.
    void WriteSaveDataFileSystemExtraData(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::WriteSaveDataFileSystemExtraData: ignorado");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 61 e 62: o leitor de informação dos saves.
    void OpenSaveDataInfoReaderBySaveDataSpaceId(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenSaveDataInfoReaderBySaveDataSpaceId: vazio");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ISaveDataInfoReader>(ctx,
                                                 std::make_shared<ISaveDataInfoReader>(system));
    }

    void OpenSaveDataInfoReaderOnlyCacheStorage(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenSaveDataInfoReaderOnlyCacheStorage: vazio");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ISaveDataInfoReader>(ctx,
                                                 std::make_shared<ISaveDataInfoReader>(system));
    }

    // 67: procurar saves por filtro. A lista é vazia: `Result` + o
    // número (`Out<s64>`).
    void FindSaveDataWithFilter(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::FindSaveDataWithFilter: nenhum save");
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::int64_t>(0);
    }

    // 69, 70 e 71: os dados extra, por atributo de save.
    void ReadSaveDataFileSystemExtraDataBySaveDataAttribute(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS,
                  "fsp-srv::ReadSaveDataFileSystemExtraDataBySaveDataAttribute: vazio");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS,
                  "fsp-srv::WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute: ignorado");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS,
                  "fsp-srv::ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute: vazio");
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 83: o proibidor de transferências. Interface vazia — que é o que
    // o Eden tem (sem comandos).
    void OpenSaveDataTransferProhibiter(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenSaveDataTransferProhibiter: vazio");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ISaveDataTransferProhibiter>(
            ctx, std::make_shared<ISaveDataTransferProhibiter>(system));
    }

    // 200: o armazenamento de dados do processo (o "romfs"). O Eden
    // abre o romfs real; nós abrimos um armazenamento **vazio**.
    void OpenDataStorageByCurrentProcess(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenDataStorageByCurrentProcess: vazio (sem romfs)");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IStorage>(ctx, std::make_shared<IStorage>(system));
    }

    // 1003 a 1018: as opções de depuração e o registo de acessos.
    void DisableAutoSaveDataCreation(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetGlobalAccessLogMode(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 1005: o modo de registo. `Out<AccessLogMode>` — um `u32`.
    void GetGlobalAccessLogMode(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<std::uint32_t>(0);
    }

    void OutputAccessLogToSdCard(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 1011: o índice do programa para o registo. `Out<u32>`.
    void GetProgramIndexForAccessLog(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<std::uint32_t>(0);
    }

    void FlushAccessLogOnSdCard(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void OutputApplicationInfoAccessLog(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetDebugOption(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 1200: o gestor de commits múltiplos.
    void OpenMultiCommitManager(HLERequestContext& ctx) {
        LOG_DEBUG(Service_FS, "fsp-srv::OpenMultiCommitManager: vazio");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IMultiCommitManager>(ctx,
                                                 std::make_shared<IMultiCommitManager>(system));
    }
};

} // namespace

// Regista o `fsp-srv` no `sm:` — o mesmo caminho do `appletOE` e do
// `time:u`.
void registar_fsp_srv(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("fsp-srv", std::make_shared<FspSrv>(system));
    Service::ServerManager::RunServer(std::move(server_manager));
}

} // namespace g610::services
