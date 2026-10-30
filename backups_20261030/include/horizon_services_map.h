// include/horizon_services_map.h
//
// As duas sessoes que o run do Pokemon Shield usa por SendSyncRequest
// (svc 0x21) na rotina:
//
//     sessao 0x981f1  =  fsp-srv (IFileSystemProxy, com dominio)   ~58 Hz
//     sessao 0xa01f0  =  nvdrv    (nvidia, CMIF simples)           ~18 Hz
//
// FECHADO em 23/10 com tres correntes independentes que concordam:
//
//   1. o NOME vem do proprio sm: — o Eden despeja os pedidos GetService
//      ([eden:W] Service::SM: GetService: nome lido = "...", LOGR da
//      board). A cadeia medida no log: GetService("fsp-srv") na linha
//      1667 e os handles 0x881f3/0x901f2/0x981f1 nascem logo a seguir
//      (linhas 1743-1831); GetService("nvdrv") na linha 2156 e 0xa01f0
//      nasce na 2194. O nome nao mora na mesa do despachante — o
//      `service_name_for_tracer` devolve vazio para as sessoes do Eden.
//
//   2. o ATENDIMENTO concorda: cada envio a 0x981f1 e' seguido por
//      `[eden:W] Service: IStorage: comando 0 (Read)` (473 no LOGR) e
//      cada envio a 0xa01f0 por
//      `[eden:W] Service: nvdrv: comando 1 (Ioctl)` (128), comando 0
//      (Open, 4) e comando 4 (QueryEvent, 2).
//
//   3. os MethodId medidos nos corpos batem nas tabelas publicas — ver
//      kSessoes abaixo. O "comando 0x10/0xC0B" do diario era um ERRO de
//      leitura: o word1 do request e' o Header1 (RawCount bits 0-9 e
//      ReceiveListCount bits 10-13), NAO o id de comando.

#pragma once

#include <cstdint>

namespace g610 {
namespace hsvc {

// ---------------------------------------------------------------------------
// O svc: verificado (id em include/g610/svc_dispatcher.hpp; o stub do
// patcher em sdk.text 0x83b0bb64 e' `brk #0x21` — o imm preserva o id)
// ---------------------------------------------------------------------------
enum : std::uint32_t {
    SwSvcSendSyncRequest               = 0x21,  // 33 — o trap #33 dos logs
    SwSvcSendSyncRequestWithUserBuffer = 0x22,  // 34 — o stub 0x83b0bb6c
};

// ---------------------------------------------------------------------------
// A decodificacao HIPC (Switchbrew, pagina "HIPC", wikitexto lido em
// 23/10/2026; a pagina antiga chama-se IPC_Marshalling e redireciona a
// HIPC). O buffer vive no Thread Local Region do guest (o x1 =
// 0x88000000 que o guest passa ao SendSyncRequest):
//
//   0x00 HeaderData (0x8): W0 = Header0Tag, W1 = Header1Tag
//   0x08 SpecialTag + pid/copy-handles/move-handles (se Header1 bit31)
//   0x?? PointerData (0x8 cada), SendData/ReceiveData/ExchangeData
//        (MapData, 0xC cada), por ordem, conforme os counts
//   0x?? RawData (RawCount words) — aqui vive o CMIF
//   0x?? ReceiveListData (0x8 cada, se ReceiveListCount > 0)
//
// Header0Tag: bits 0-15 = Tag (MessageType: 6 = Invoke2Method [5.0.0+],
//   7 = Invoke2ManagerMethod); 16-19 PointerCount; 20-23 SendCount;
//   24-27 ReceiveCount; 28-31 ExchangeCount.
// Header1Tag: bits 0-9 RawCount em words; 10-13 ReceiveListCount;
//   20-30 ReceiveListOffset; 31 SpecialCount.
// SpecialTag: bit 0 = Pid; bits 1-4 CopyHandleCount; bits 5-8
//   MoveHandleCount. Pid ocupa 0x8 bytes depois do SpecialTag.
//
// Os valores MEDIDOS das duas rotinas:
//     0x00000006  = Tag 6, sem descritores
//     0x01000006  = Tag 6, ReceiveCount = 1      (o C-buf do Read)
//     0x01110006  = Tag 6, PointerCount = 1 + SendCount = 1
//                   (o par que o HipcAutoSelect cria: um dos dois e'
//                   NULL de tamanho 0, o outro traz o valor real)
//   — o "cmd 0x10/0xC0B" do diario = o Header1 medido (RawCount 12 e
//   raw 11 + ReceiveListCount 3), nunca foi um id de comando.
// ---------------------------------------------------------------------------

// O CMIF dentro do RawData (Switchbrew HIPC + o `vendor/eden/core/hle/
// ipc.h` — DataPayloadHeader e DomainMessageHeader do upstream):
//
//   Padding de 16 bytes — o total de padding dentro do raw e' SEMPRE
//   0x10; se o raw comeca a 0x8 faz 8 bytes antes e 0x14 (pad) etc.
//   CmifMessage: CmifInHeader (0x10) { "SFCI", version=1, MethodId,
//                Token } + os parametros de entrada.
//   CmifDomain (se a sessao serve um dominio): o CmifDomainMessageIn-
//   Header (0x10) vem ANTES do CmifMessage, no desenho medido a 0x10
//   (o campo {kind u8, InObjectCount u8, InRawSize u16} fica comprimido
//   na primeira palavra: 0x00100001 = kind=1, objs=0, rawsize=16):
//     { RequestKind (1 = InvokeMethod, 2 = Release), InObjectCount,
//       InRawSize, TargetObjectId, Reserved, Token }
//     + CmifMessage de tamanho InRawSize
//     + array de CmifDomainObjectId (InObjectCount)
//   Depois: os tamanhos (u16) do buffer type 0xA (OutPointer).
//
// SFCI = 0x49434653 (a assinatura do pedido), SFCO = 0x4f434653 (a
// resposta; [14.0.0+] o campo a +0xC do SFCO = InterfaceId = sha256 do
// nome da interface).
//
// Medido no LOGQ/LOGR da board (o on_trap le 24 words = 96 bytes de
// x1 desde a obra de 23/10):
//
//   rotina de 0x981f1 (a forma (b): 182 dos 327 sends):
//     [01000006][00000010][size-lo][addr-lo][tag mapa]{0,0,0}
//     [00200001][objId {2|3}][0][0] ['SFCI'][1][0=Read][0]
//     [option=0][offset-lo][offset-hi=2][size]
//   = Read de IFile/IStorage por dominio: TargetObjectId {2|3}, o
//   InRawSize=32, MethodId 0 = Read (Switchbrew "Filesystem services":
//   IStorage::Read/IFile::Read = cmd 0, "type 0x46 buffer, offset and
//   length"). O size-lo do MapData = o tamanho do read (0x50=80,
//   0xf94=3988, 0x88b4, 0x28bac, 0x20fac8...) e e' o mesmo valor que
//   o Eden imprime (`IStorage::Read: offset=0X22D853950
//   length=27148` — o 27148 = 0x6a0c e' o caso comum medido antes).
//   Primeiro read de cada objeto = 80 bytes no pointer-buffer
//   (addr 0x3_0FFFE9xx).
//
//   chamada de init de 0x981f1 (a forma (a): 3 sends no run):
//     [00000006][0000000c][0][0][00100001][obj=1][0][0]
//     ['SFCI'][1][MethodId][0]['SFCI'][1]...
//   MethodId 0x3ED = 1005 = GetGlobalAccessLogMode e 0xC8 = 200 =
//   OpenDataStorageByCurrentProcess (a tabela IFileSystemProxy) — o
//   open que cria os objectos de dominio 2/3 que a rotina le. A cauda
//   ['SFCI'][1] dos 12 words do raw esta' medida mas sem interpretacao
//   decidida; nao interfere na etiqueta da trap.
//
//   rotina de 0xa01f0 (nvdrv):
//     [01110006][00000c0b][ptr-{size|addr}] [send nulo 0,0] [0]
//     ['SFCI'][1][MethodId][0][args...]
//   MethodId {1 = Ioctl, 0 = Open, 4 = QueryEvent} — a tabela "NV
//   services" do Switchbrew (nv: Open/Ioctl/QueryEvent). O PointerData
//   = o par HipcAutoSelect (send NULL de tamanho 0; pointer com
//   size {0xb0=176|0x18=24|0=0|0x28=40} e addr 0x0FFFExxx — o
//   pointer-buffer do processo). O Header1 0xC0B = raw 11 words +
//   ReceiveListCount 3 (as listas de receive do lado do server).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// As sessoes medidas. Os campos ficam no mesmo pacote que o run mostra
// para o despachante etiquetar as traps sem derivar logica de palpite:
// o (handle, w0) identifica a sessao; `metodo_id` = o MethodId CMIF
// dominante da rotina.
// ---------------------------------------------------------------------------
struct SessaoMedida {
    std::uint64_t handle;      // o x0 que o guest passa
    const char*   servico;     // fixado pelo nome no GetService (LOGR)
    const char*   interface_;  // a interface no topo da sessao
    std::uint32_t w0;          // o Header0 dominante da rotina
    std::uint32_t metodo_id;   // o MethodId CMIF dominante
    std::uint32_t hz;          // cadencia medida (LOGK, jitter de ~15 s)
    std::uint32_t envios;      // na janela do LOGK
};

constexpr SessaoMedida kSessoes[] = {
    {0x981f1ull, "fsp-srv", "nn::fssrv::sf::IFileSystemProxy (domain)",
     0x01000006u, 0x00000000u /* Read */, 58u, 866u},
    {0xa01f0ull, "nvdrv", "nv: Open/Ioctl/QueryEvent (tabela NV services)",
     0x01110006u, 0x00000001u /* Ioctl */, 18u, 264u},
};
constexpr int kNumSessoes = 2;

// ---------------------------------------------------------------------------
// Os MethodId por sessao, com a fonte citada. Tudo CONFIRMADO contra as
// duas fontes publicas (Switchbrew hoje) e os atendimentos medidos no
// Eden da board — nada aqui e' palpite.
// ---------------------------------------------------------------------------
struct MetodoMedido {
    std::uint32_t id;
    const char*   nome;
    const char*   onde;      // a medida que o fechou
};

constexpr MetodoMedido kMetodosFspSrv[] = {
    {0x00000000u, "IFile::Read / IStorage::Read (por dominio, obj 2/3)",
     "LOGQ/LOGR: 182 sends; Eden imprime `comando 0 (Read)`; Switchbrew "
     "Filesystem services: IStorage cmds 0 = Read"},
    {0x000003EDu, "IFileSystemProxy GetGlobalAccessLogMode",
     "LOGR: 3 sends no arranque da sessao; Switchbrew: IFileSystemProxy 1005 "
     "= GetGlobalAccessLogMode"},
    {0x000000C8u, "IFileSystemProxy OpenDataStorageByCurrentProcess",
     "LOGR: 3 sends no arranque da sessao; Eden serve IStorage depois; "
     "Switchbrew: IFileSystemProxy 200 = OpenDataStorageByCurrentProcess"},
};
constexpr int kNumMetodosFspSrv = 3;

constexpr MetodoMedido kMetodosNvDrv[] = {
    {0x00000001u, "Ioctl",
     "LOGR: `nvdrv: comando 1 (Ioctl)` 128x; Switchbrew NV services"},
    {0x00000000u, "Open",
     "LOGR: `nvdrv: comando 0 (Open)` 4x"},
    {0x00000004u, "QueryEvent",
     "LOGR: `nvdrv: comando 4 (QueryEvent)` 2x"},
    {0x00000002u, "Close",
     "LOGN: MethodId 2 no corpo; tabela NV services = 2 Close"},
    {0x00000005u, "MapSharedMem",
     "LOGT2 (23/10): method=0x5 no run (1x); tabela NV services = 5 "
     "MapSharedMem"},
};
constexpr int kNumMetodosNvDrv = 4;

// ---------------------------------------------------------------------------
// Como o despachante deve consumir este mapa (padrao do codigo):
//   - o (handle, w0) do request cruza kSessoes[]: quando bater, a trap
//     ganha a etiqueta (servico + MethodId esperado) no log, sem nenhum
//     caminho de despacho novo;
//   - a leitura completa do request (dominio/params) exige os 96 bytes
//     que o on_trap ja' le (tests/boot_trace.cpp: o print `[ipc ] #33`
//     de 24 words).
// ---------------------------------------------------------------------------

}  // namespace hsvc
}  // namespace g610
