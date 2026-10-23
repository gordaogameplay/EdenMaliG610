// include/horizon_services_map.h
//
// Mapeamento TEORICO (sem confirmacao) dos dois pares sessao+comando que o
// run longo do Pokemon Shield gera por SendSyncRequest (svc 0x21):
//
//     sessao 0x981f1, comando 0x10  (16)     ~58 Hz
//     sessao 0xa01f0, comando 0xC0B (3079)   ~18 Hz
//
// Tudo aqui e' CANDIDATO: um id de comando so' se fecha contra as tabelas
// publicas (Switchbrew, "Services IPC" por servico; o Ryujinx e' a segunda
// fonte). Este header existe para o despachante de traps poder etiquetar
// as traps com o que ja' e' medido, sem derivar logica de meros palpites.
//
// Fonte das medidas: os prints `[ipc ] #33` e `[svc #...]` do on_trap
// (tests/boot_trace.cpp) no log LOGK da board — o run longo do Pokemon
// Shield (janela de ~15 s). O on_trap le 24 bytes da mensagem via
// g_mem->read.

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
// O que esta' MEDIDO no run (LOGK, janela de ~15 s; ver PROGRESSO.md de 23/10):
//
//   request: word0 = desc do HIPC; word1 = o id do comando;
//            word0 medido 0x01000006 (sessao 57 Hz) e 0x01110006 ?? (16 Hz)
//            — a diferente no bit 12 pode ser flag de domain/desc e esta
//            POR CONFIRMAR;
//   corpos (os 6 words que o on_trap le em [x1 = 0x88000000 = o IPC/TLS
//            do guest]):
//            rotina da 57 Hz: [01000006][0010][token variavel][ptr 0xd9b9xxxx][1][0]
//            rotina da 16 Hz: [01110006][c0b][tag {283000|282000|202000|82000}][0fffe5xx][0][0]
//            A chamada DE SETUP de cada sessao (a primeira) e' diferente:
//            os pares {0, 0x0000000E} e {6, 0x0000000C} do diario = os
//            words W4/W5 dessa chamada de setup (nao e' "resposta"!) —
//            o 0xE/0xC tem a cara de conta de recv-list do descriptor A
//            (os prints `pedido em ... type=6 unk=0x8000000E`, a outra
//            familia de print do boot_trace.cpp:274, tem o mesmo padrao).
// ---------------------------------------------------------------------------
struct CmdMedida {
    std::uint64_t handle;      // o x0 que o guest passa
    std::uint32_t cmd_id;      // o word1 do request
    std::uint32_t hz;          // cadencia medida (arredondada)
    std::uint32_t envios;      // na janela do LOGK (~15 s)
    std::uint32_t req_w0;      // o desc medido
    std::uint32_t msg[4];      // W2..W5 (a forma mais comum; W2/W3 mudam por
                               // chamada — o token de 981f1, a tag de a01f0)
};

constexpr CmdMedida kMedidas[] = {
    {0x981f1ull, 0x10u,  58u, 866u, 0x01000006u,
     {0x00006a0cu, 0xd9b8b480u, 0x00000001u, 0x00000000u}},
    {0xa01f0ull, 0xC0Bu, 18u, 264u, 0x01110006u,
     {0x00283000u, 0x0fffe588u, 0x00000000u, 0x00000000u}},
};
constexpr int kNumMedidas = 2;

// ---------------------------------------------------------------------------
// Os stamps CMIF, medidos nas OUTRAS sessoes do mesmo run: os prints
// `[ipc ] #33` de handles como 0x101fe/0x181fd (o trafego do boot e de
// outras threads, ~184 sends no LOGK) tem nos W4 os stamps `SFCI`
// (0x49434653 — bytes "SFCI", o site do request) e `SFCO` (0x4f434653 —
// bytes "SFCO", o stamp documentado do lado da resposta; medido tambem
// em requests de outras sessoes, POR CONFIRMAR). E' o formato CMIF
// (nn::sf::cmif) do sdk da Nintendo, ver em Switchbrew ("IPC
// Marshalling") e no Ryujinx.
//
// Nas DUAS irma (0x981f1/0xa01f0) nenhum stamp aparece nos W0..W5: ou o
// stamp delas fica alem da janela de 24 bytes que o on_trap le', ou o
// corpo delas usa outra familia do formato. Pista por fechar.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Os candidatos por comando. TODOS NAO-CONFIRMADOS (confianca 0): servem
// para orientar a proxima sessao, nao para decidir caminhos no despachante.
//
// Como fechar o id amanha (checklist):
//
//   1. desc do request: o word0 ja' esta' medido inteiro (0x01000006 vs
//      0x01110006) — falta abrir a secao: o delta 0x00110000 (os bits 16
//      e 20) tem contas no formato HIPC (counts de statics/buffers);
//      confirmar em Switchbrew fecha o item;
//   2. quem abre a sessao: os dois handles nascem cedo no boot (servem
//      primeiro em [svc #234] e [svc #274] do LOGK) — o nome do servico
//      do CreateSession que devolveu 0x981f1 e 0xa01f0 fecha o SERVICO;
//   3. com o servico fechado, bater o id de comando na tabela publica
//      (Switchbrew por servico; Ryujinx como segunda fonte) e deixar o
//      resultado CONFIRMAR no header com a fonte citada no comentario.
// ---------------------------------------------------------------------------
struct CmdCandidato {
    const char* servico;   // interface candidata
    const char* observ;    // porque e' candidata (a pista medida)
    const char* fonte;     // onde confirmar
};

// cmd 0x10 (16) @ 58 Hz: uma leitura por frame. A cadencia ~58 Hz e'
// a pista mais forte: estados por frame tendem para input/display.
// Candidatos DENTRO dessa cadencia (PONTO ABERTO: os W4/W5 da rotina,
// {1, 0}, nao fechem decoder nenhum ainda):
constexpr CmdCandidato kCandidatos0x10[] = {
    {"hid/IAppletResource", "poll por frame em cima do shared memory",
     "Switchbrew: HID services"},
    {"hid/IHidSystemServer", "estado por frame",
     "Switchbrew: HID services"},
    {"vi/IApplicationDisplayService", "poll de estado de display",
     "Switchbrew: VI services"},
    {"am/ILibraryAppletAccessor", "estado de applet",
     "Switchbrew: AM services"},
};
constexpr int kNumCandidatos0x10 = 4;

// cmd 0xC0B (3079) @ 18 Hz: id ALTO. Em tabelas do vendor, ids acima de
// 0x0400 aparecem mais em interfaces de applet/servicos internos. A tag
// {283000|282000|202000|82000} no W2 (4 valores em rotacao) e' a parte
// mais estranha do corpo medido — sem decoder ainda.
constexpr CmdCandidato kCandidatos0xC0B[] = {
    {"appletAE/IAppletAccessor", "id alto de applet",
     "Switchbrew: AppletAE services"},
    {"am/ILibraryAppletAccessor", "id alto de applet",
     "Switchbrew: AM services"},
    {"hid/IHidServer", "id alto possivel na interface de introspeccao",
     "Switchbrew: HID services"},
};
constexpr int kNumCandidatos0xC0B = 3;

// ---------------------------------------------------------------------------
// Como o despachante deve consumir este mapa (padrao do codigo):
//   - o cmd_id do request cruza kMedidas[] por (handle, cmd_id): quando
//     bater, a trap ganha uma etiqueta curta para o log (o handle e a
//     cadencia esperada) sem nenhum caminho de despacho novo;
//   - os candidatos ficam FORA da logica: nenhuma decision com
//     confianca==0 dentro do despachante — servem de anotacao no log
//     ate' uma sessao confirmar.
// ---------------------------------------------------------------------------

}  // namespace hsvc
}  // namespace g610
