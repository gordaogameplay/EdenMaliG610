// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
//
// O `time:u` — o tempo do sistema, **nosso**.
//
// ## O que é
//
// O SDK, no arranque, pede `GetService("time:u")` ao `sm:` — é o que
// inicializa o `nn::time`. Sem o serviço, o `sm:` **defere** o pedido
// (`sm.cpp:200-203: ctx.SetIsDeferred()`), a sessão fica pendurada e —
// medido no boot_p6b (linha 2444) — o `m_current_request` do
// `KServerSession` do `sm:` fica **preso para sempre**:
//
//     sessao=0xaaab0e1171b0 req=1 corrente=0xaaab0e11ab90 sinal=0
//
// `IsSignaled` exige `m_current_request == nullptr`
// (`k_server_session.cpp:1391`), logo a sessão nunca mais sinaliza, a
// pump vê `req=1` e não pode pegar, o thread do guest fica estacionado
// no `SendSyncRequest` (`BeginWait`) por toda a vida, e o SDK morre no
// `IApplicationFunctions::SetTerminateResult` → `svc #0x26 Break` →
// exit 124.
//
// O cap é a **raiz**: registar o `time:u`.
//
// ## De onde vem a tabela
//
// Do Eden futuro — `core/hle/service/psc/time/`
// (raw.githubusercontent.com/emuall/eden/master): `static.cpp` (a
// tabela do `time:u`, 19 comandos), `system_clock.cpp` (ISystemClock,
// 0..4), `steady_clock.cpp` (ISteadyClock, 0/2/3/100/101/102/200) e
// `time_zone_service.cpp` (ITimeZoneService,
// 0/1/2/3/4/5/6/7/8/20/100/101/201/202). Os IDs e os nomes são copiados
// à mão na forma da casa (`applet_oe.cpp`), porque as tabelas **não se
// inventam**.
//
// ## Quanto do módulo se copia
//
// Nenhum corpo do Eden: o módulo `psc/time` é uma hierarquia inteira
// (`TimeManager`, os `*SystemClockCore`, `SharedMemory`, `TimeZone`…)
// que arrasta a lib `tz` e o `KSharedMemory` — este último **nem
// existe** no kernel vendorizado. O que o guest precisa no arranque é
// a *interface* e a resposta honesta aos comandos. Por isso:
//
//   - as **tabelas** são as do Eden (os ids a fio);
//   - o **relógio vive de uma única fonte** — o segundo-de-epoch do
//     board — e a unidade está medida em
//     `standard_steady_clock_core.cpp`:
//     `duration_cast<std::chrono::seconds>` — o `time_point` é em
//     **segundos**; e o posix do Eden é
//     `GetTimeFromTimePointAndContext`: `offset + steady.time_point`;
//   - a **falta de memória partilhada** (comando 20 do `time:u`)
//     responde com a forma certa e **um log** — a lista de peças em
//     falta é o que nos guia; a medição seguinte diz se o guest a
//     precisa;
//   - comandos que o Eden devolve `ResultNotImplemented`
//     (`SetStandardSteadyClockInternalOffset` (50),
//     `GetStandardSteadyClockRtcValue` (51), `LoadLocationNameList`
//     (3), `LoadTimeZoneRule` (4)) recebem a regra da casa
//     d'`applet_oe.cpp`: **nunca um Result de erro** — sucesso com os
//     valores mínimos coerentes.
//
// A medição (*medir → incluir comando → medir*) decide o que vem
// depois. Isto é a **primeira peça**: fazer o `GetService("time:u")`
// responder.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
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
// As estruturas do fio — copiadas de `psc/time/common.h`
// ─────────────────────────────────────────────────────────────────────────────
//
// Não se negociam: os offsets binários do CMIF são o contrato com o
// guest. Os `static_assert` são os do Eden, à letra.

using ClockSourceId = std::array<u8, 16>;

struct SteadyClockTimePoint {
    bool IdMatches(const SteadyClockTimePoint& outra) const {
        return clock_source_id == outra.clock_source_id;
    }
    // O Eden Common::PSC::Time (common.h) tem `operator== = default` —
    // os snapshots comparam contextos inteiros.
    bool operator==(const SteadyClockTimePoint&) const = default;

    s64 time_point;
    ClockSourceId clock_source_id;
};
static_assert(sizeof(SteadyClockTimePoint) == 0x18);

struct SystemClockContext {
    bool operator==(const SystemClockContext&) const = default;

    s64 offset;
    SteadyClockTimePoint steady_time_point;
};
static_assert(sizeof(SystemClockContext) == 0x20);

struct CalendarTime {
    s16 year;
    s8 month;
    s8 day;
    s8 hour;
    s8 minute;
    s8 second;
};
static_assert(sizeof(CalendarTime) == 0x8);

struct CalendarAdditionalInfo {
    s32 day_of_week;
    s32 day_of_year;
    std::array<char, 8> name;
    s32 is_dst;
    s32 ut_offset;
};
static_assert(sizeof(CalendarAdditionalInfo) == 0x18);

using LocationName = std::array<char, 0x24>;
static_assert(sizeof(LocationName) == 0x24);

using RuleVersion = std::array<char, 0x10>;
static_assert(sizeof(RuleVersion) == 0x10);

// `psc/time/common.h` — os três tipos de relógio que um snapshot pode
// relatar. O guest manda-o como palavra normal no comando 400.
enum class TimeType : u8 {
    UserSystemClock = 0,
    NetworkSystemClock = 1,
    LocalSystemClock = 2,
};

struct ClockSnapshot {
    SystemClockContext user_context;
    SystemClockContext network_context;
    s64 user_time;
    s64 network_time;
    CalendarTime user_calendar_time;
    CalendarTime network_calendar_time;
    CalendarAdditionalInfo user_calendar_additional_time;
    CalendarAdditionalInfo network_calendar_additional_time;
    SteadyClockTimePoint steady_clock_time_point;
    LocationName location_name;
    bool is_automatic_correction_enabled;
    TimeType type;
    u16 unk_CE;
};
static_assert(sizeof(ClockSnapshot) == 0xD0,
              "ClockSnapshot: o tamanho é o contrato com o guest");

// ─────────────────────────────────────────────────────────────────────────────
// O estado dos relógios
// ─────────────────────────────────────────────────────────────────────────────
//
// Uma única fonte de verdade: o epoch do board, em segundos
// (`std::chrono::system_clock`). Todos os relógios (user, network,
// local, ephemeral) partem deste pé; o `ajuste_s` é o que
// `SetCurrentTime` e `SetSystemClockContext` deixam, e o posix de
// qualquer relógio é `agora() + ajuste_s`.
//
// O contexto do Eden é `{ offset, steady_time_point }` e o posix é
// `offset + steady.time_point` (segundos). A nossa derivação, para a
// fórmula bater sempre:
//
//   - `SetCurrentTime(t)`: `ajuste = t - agora` ⇒
//     contexto `{ t - agora_foto, { agora_foto, fonte } }` — e a soma
//     da fórmula do Eden dá exactamente `t`, no instante — e
//     `epoch + ajuste` no próximo instante;
//   - `SetSystemClockContext(ctx)`: deriva o ajuste do mesmo passo
//     inverso (a fórmula inteira está abaixo). **Aceita-se sempre**,
//     com log se a fonte não bater — o guest calculou este contexto a
//     partir dos que **nós** devolvemos, e um Result de erro aqui é um
//     abort no SDK (a regra da casa d'`applet_oe.cpp`).
//
// O `source_id` é fixo: um literal escrito à mão. No Eden é o
// `DEFAULT_SOURCE_ID`, um UUID qualquer — o que importa é **nunca
// mudar** entre comandos, porque é isso que o `IdMatches` compara.
struct EstadoDoRelogio {
    static ClockSourceId fonte() {
        ClockSourceId id{};
        std::memcpy(id.data(), "g610-relogio-fonte", 16);
        return id;
    }

    // O epoch do board, em **segundos** — a unidade medida na fonte.
    static s64 agora() {
        const auto desde_o_epoch = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::seconds>(desde_o_epoch).count();
    }

    // O ajuste: `agora() + ajuste_s` é o posix que o relógio diz.
    s64 ajuste_s = 0;

    // A correcção automática (comando 100). O Eden arranca-a
    // desligada no arranque do sistema — e sem rede que a sustente,
    // é o que se diz aqui.
    bool automatico = false;

    // Os offsets do relógio firme (o ISteadyClock os soma ao ponto):
    // o que o `SetTestOffset` (comando 3 do steady) e o
    // `SetStandardSteadyClockInternalOffset` (comando 50 do `time:u`)
    // deixam. Em segundos, no nosso modelo.
    s64 teste_s = 0;
    s64 interno_s = 0;

    // O contexto vivo: `{ ajuste, { agora, fonte } }`. A soma da
    // fórmula do Eden dá `agora() + ajuste_s` — que é exactamente o
    // posix vivo, por construção:
    //
    //   offset(=ajuste) + steady(agora) == posix vivo
    //
    // A corrida entre as duas leituras de `agora()` é de micros, e o
    // posix de um save tem segundo de resolução.
    SystemClockContext contexto() const {
        const s64 t = agora();
        return SystemClockContext{ajuste_s, SteadyClockTimePoint{t, fonte()}};
    }

    // Aceitar o contexto do guest e derivar o ajuste do passo inverso.
    // O guest fez `{ offset, steady }` e pretende o posix
    // `offset + steady`; o nosso `agora()` é o ponto-firme no
    // instante da adição, logo
    //
    //     ajuste = offset + steady.time_point - agora
    //
    // mantém o posix do guest no instante, e a deriva passa a ser
    // monotónica como os nossos.
    void aplicar(const SystemClockContext& contexto) {
        const s64 t = agora();
        const SteadyClockTimePoint nossa{0, fonte()};
        if (!contexto.steady_time_point.IdMatches(nossa)) {
            LOG_WARNING(Service_Time,
                        "SetSystemClockContext: fonte do guest {:02x}… nao "
                        "bate com a nossa — aceito, a medir",
                        static_cast<unsigned>(contexto.steady_time_point.clock_source_id[0]));
        }
        ajuste_s =
            contexto.offset + contexto.steady_time_point.time_point - t;
    }

    // O calendário deste posix — UTC (`gmtime`).
    //
    // O fuso do Eden é uma regra de agendamento (a lib `tz`); aqui o
    // fuso do motor é UTC **por desenho** — o board não tem regra, e
    // inventar DST é inventar tempo. Meg o `CalendarTime` **tem** de
    // voltar com dados: é o que o SDK usa para as datas dos saves.
    CalendarTime calendario(s64 posix_s) const {
        const std::time_t tempo = static_cast<std::time_t>(posix_s);
        std::tm t{};
        ::gmtime_r(&tempo, &t);
        return CalendarTime{static_cast<s16>(t.tm_year + 1900),
                            static_cast<s8>(t.tm_mon + 1),
                            static_cast<s8>(t.tm_mday),
                            static_cast<s8>(t.tm_hour),
                            static_cast<s8>(t.tm_min),
                            static_cast<s8>(t.tm_sec)};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// O calendário adicional do fuso único deste motor: `UTC`, sem DST.
// Vive fora das classes porque o snapshot do `time:u` o usa também — e
// é o MESMO valor nos dois ramos (user/network).
CalendarAdditionalInfo adicional() {
    return CalendarAdditionalInfo{0, 0, {'U', 'T', 'C'}, 0, 0};
}

// ─────────────────────────────────────────────────────────────────────────────
// `ISystemClock` — `psc/time/system_clock.cpp`
// ─────────────────────────────────────────────────────────────────────────────

class RelogioDeSistema final : public Service::ServiceFramework<RelogioDeSistema> {
public:
    RelogioDeSistema(Core::System& system, EstadoDoRelogio* estado)
        : ServiceFramework{system, "ISystemClock"}, estado_{estado} {
        // A tabela é a do Eden (`system_clock.cpp`), à letra:
        // 0 GetCurrentTime, 1 SetCurrentTime, 2 GetSystemClockContext,
        // 3 SetSystemClockContext, 4 GetOperationEventReadableHandle.
        static const FunctionInfo functions[] = {
            {0, &RelogioDeSistema::GetCurrentTime, "GetCurrentTime"},
            {1, &RelogioDeSistema::SetCurrentTime, "SetCurrentTime"},
            {2, &RelogioDeSistema::GetSystemClockContext, "GetSystemClockContext"},
            {3, &RelogioDeSistema::SetSystemClockContext, "SetSystemClockContext"},
            {4, &RelogioDeSistema::GetOperationEventReadableHandle,
             "GetOperationEventReadableHandle"},
        };
        RegisterHandlers(functions);

        // O evento de operação (comando 4). No Eden, é o evento onde o
        // guest *observa* mudanças do relógio (o relógio de rede
        // usa-o). Aqui: um evento real, não sinalizado — observação
        // sem sinal é o honesto. O padrão é o d'`applet_oe.cpp`.
        auto& kernel = system.Kernel();
        auto& processo = Kernel::GetCurrentProcess(kernel);
        evento_ = new Kernel::KEvent(kernel);
        evento_->Initialize(kernel, &processo);
    }

private:
    // 0: posix em segundos, vivo: `agora + ajuste`. A resposta é
    // resultado (2 palavras) + s64 (2 palavras).
    void GetCurrentTime(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s64>(EstadoDoRelogio::agora() + estado_->ajuste_s);
    }

    // 1: fixar o tempo.
    void SetCurrentTime(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const s64 tempo = rp.PopRaw<s64>();
        estado_->ajuste_s = tempo - EstadoDoRelogio::agora();

        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 2: o contexto vivo (0x20 = 8 palavras).
    void GetSystemClockContext(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 10};
        rb.Push(ResultSuccess);
        rb.PushRaw(estado_->contexto());
    }

    // 3: aceitar o contexto do guest (a derivação está no estado).
    void SetSystemClockContext(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto contexto = rp.PopRaw<SystemClockContext>();
        estado_->aplicar(contexto);

        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 4: o evento real — o mesmo caminho do `GetEventHandle`
    // d'`applet_oe.cpp` (um handle de cópia).
    void GetOperationEventReadableHandle(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &evento_->GetReadableEvent());
    }

    Kernel::KEvent* evento_ = nullptr;
    EstadoDoRelogio* estado_;
};

// ─────────────────────────────────────────────────────────────────────────────
// `ISteadyClock` — `psc/time/steady_clock.cpp`
// ─────────────────────────────────────────────────────────────────────────────

class RelogioSteady final : public Service::ServiceFramework<RelogioSteady> {
public:
    RelogioSteady(Core::System& system, EstadoDoRelogio* estado)
        : ServiceFramework{system, "ISteadyClock"}, estado_{estado} {
        // A tabela é a do Eden (`steady_clock.cpp`): 0
        // GetCurrentTimePoint, 2 GetTestOffset, 3 SetTestOffset, 100
        // GetRtcValue, 101 IsRtcResetDetected, 102 GetSetupResultValue,
        // 200 GetInternalOffset.
        static const FunctionInfo functions[] = {
            {0, &RelogioSteady::GetCurrentTimePoint, "GetCurrentTimePoint"},
            {2, &RelogioSteady::GetTestOffset, "GetTestOffset"},
            {3, &RelogioSteady::SetTestOffset, "SetTestOffset"},
            {100, &RelogioSteady::GetRtcValue, "GetRtcValue"},
            {101, &RelogioSteady::IsRtcResetDetected, "IsRtcResetDetected"},
            {102, &RelogioSteady::GetSetupResultValue, "GetSetupResultValue"},
            {200, &RelogioSteady::GetInternalOffset, "GetInternalOffset"},
        };
        RegisterHandlers(functions);
    }

private:
    // O ponto-firme é `agora + teste + interno`, em segundos — é o que
    // o `SteadyClockCore::GetCurrentTimePoint` do Eden faz, que soma
    // teste + interno ao ponto.
    void GetCurrentTimePoint(HLERequestContext& ctx) {
        const s64 ponto =
            EstadoDoRelogio::agora() + estado_->teste_s + estado_->interno_s;
        ResponseBuilder rb{ctx, 8};
        rb.Push(ResultSuccess);
        rb.PushRaw(SteadyClockTimePoint{ponto, EstadoDoRelogio::fonte()});
    }

    void GetTestOffset(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s64>(estado_->teste_s);
    }

    void SetTestOffset(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        estado_->teste_s = rp.PopRaw<s64>();
        RespondeOK(ctx);
    }

    static void RespondeOK(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 100: o valor do RTC — no Eden é o chip real; aqui é o epoch do
    // board.
    void GetRtcValue(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s64>(EstadoDoRelogio::agora());
    }

    // 101: o RTC não tem aqui um reset que se meça — falso (1 palavra).
    void IsRtcResetDetected(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<bool>(false);
    }

    // 102: o resultado da montagem do steady no arranque — o Eden
    // guarda `ResultSuccess` (`SetSetupResultValue`); a raw 0.
    void GetSetupResultValue(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0);
    }

    void GetInternalOffset(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s64>(estado_->interno_s);
    }

    EstadoDoRelogio* estado_;
};

// ─────────────────────────────────────────────────────────────────────────────
// `ITimeZoneService` — `psc/time/time_zone_service.cpp`
// ─────────────────────────────────────────────────────────────────────────────
//
// O fuso do motor é **UTC**: o board não tem regra de agendamento, e
// inventar DST é inventar tempo. Mas os calendários do
// `ToCalendarTime*` têm de **voltar com dados** (as datas dos saves) —
// e voltam, por `gmtime`.
class FusoHorario final : public Service::ServiceFramework<FusoHorario> {
public:
    FusoHorario(Core::System& system, EstadoDoRelogio* estado)
        : ServiceFramework{system, "ITimeZoneService"}, estado_{estado} {
        // A tabela é a do Eden (`time_zone_service.cpp`):
        // 0/1/2/3/4/5/6/7/8/20/100/101/201/202.
        static const FunctionInfo functions[] = {
            {0, &FusoHorario::GetDeviceLocationName, "GetDeviceLocationName"},
            {1, &FusoHorario::SetDeviceLocationName, "SetDeviceLocationName"},
            {2, &FusoHorario::GetTotalLocationNameCount,
             "GetTotalLocationNameCount"},
            {3, &FusoHorario::LoadLocationNameList, "LoadLocationNameList"},
            {4, &FusoHorario::LoadTimeZoneRule, "LoadTimeZoneRule"},
            {5, &FusoHorario::GetTimeZoneRuleVersion, "GetTimeZoneRuleVersion"},
            {6, &FusoHorario::GetDeviceLocationNameAndUpdatedTime,
             "GetDeviceLocationNameAndUpdatedTime"},
            {7, &FusoHorario::SetDeviceLocationNameWithTimeZoneRule,
             "SetDeviceLocationNameWithTimeZoneRule"},
            {8, &FusoHorario::ParseTimeZoneBinary, "ParseTimeZoneBinary"},
            {20, &FusoHorario::GetDeviceLocationNameOperationEventReadableHandle,
             "GetDeviceLocationNameOperationEventReadableHandle"},
            {100, &FusoHorario::ToCalendarTime, "ToCalendarTime"},
            {101, &FusoHorario::ToCalendarTimeWithMyRule,
             "ToCalendarTimeWithMyRule"},
            {201, &FusoHorario::ToPosixTime, "ToPosixTime"},
            {202, &FusoHorario::ToPosixTimeWithMyRule, "ToPosixTimeWithMyRule"},
        };
        RegisterHandlers(functions);

        // O evento (comando 20). O Eden devolve aqui
        // `ResultNotImplemented` — um evento morto é melhor que um
        // handle vazio; segue o padrão do ISystemClock:4.
        auto& kernel = system.Kernel();
        auto& processo = Kernel::GetCurrentProcess(kernel);
        evento_ = new Kernel::KEvent(kernel);
        evento_->Initialize(kernel, &processo);
    }

private:
    // O nome do fuso. `LocationName` = 0x24 bytes (0x23 chars + '\0'),
    // 9 palavras na resposta.
    LocationName nome() const {
        LocationName retencao{};
        const std::size_t caber = std::min(retencao.size() - 1, local_.size());
        std::memcpy(retencao.data(), local_.data(), caber);
        return retencao;
    }

    void GetDeviceLocationName(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 11};
        rb.Push(ResultSuccess);
        rb.PushRaw(nome());
    }

    void SetDeviceLocationName(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto pedido = rp.PopRaw<LocationName>();
        local_.assign(pedido.begin(), pedido.end());

        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 2: a lista deste serviço tem um nome — o fuso do motor (é o que
    // este serviço conhece).
    void GetTotalLocationNameCount(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(1);
    }

    // 3: o Eden devolve NotImplemented; a casa responde a lista com o
    // fuso que este serviço conhece (um nome), count 1.
    void LoadLocationNameList(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto indice = rp.PopRaw<u32>();
        (void)indice;

        ctx.WriteBuffer(nome());

        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(1);
    }

    // 4: o Eden devolve NotImplemented. A regra devolvida é **zeros**
    // — este motor não sabe agendamento DST e os calendários passam
    // pela UTC. O guest aloca o buffer da regra (a lib `tz` do lado
    // dele); enche-se tudo de zeros com o tamanho do buffer — o
    // `WriteBuffer` cap já sê-lo faria, mas a cópia fica explicita.
    void LoadTimeZoneRule(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto pedido = rp.PopRaw<LocationName>();
        local_.assign(pedido.begin(), pedido.end());

        const std::size_t tamanho = ctx.GetWriteBufferSize(0);
        if (tamanho > 0) {
            const std::vector<u8> zeros(tamanho, 0);
            ctx.WriteBuffer(zeros.data(), tamanho);
        }

        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 5: a versão da regra — a do Eden vem do `tz` do sistema (o
    // literal "v1.0.0" é honesto e 0x10 bytes).
    void GetTimeZoneRuleVersion(HLERequestContext& ctx) {
        RuleVersion version{};
        std::memcpy(version.data(), "v1.0.0", 6);
        ResponseBuilder rb{ctx, 6};
        rb.Push(ResultSuccess);
        rb.PushRaw(version);
    }

    // 6: nome (9 palavras) + ponto-firme (0x18 = 6 palavras) = 2 + 9 + 6
    // = 17 palavras.
    void GetDeviceLocationNameAndUpdatedTime(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 17};
        rb.Push(ResultSuccess);
        rb.PushRaw(nome());
        rb.PushRaw(SteadyClockTimePoint{EstadoDoRelogio::agora(),
                                        EstadoDoRelogio::fonte()});
    }

    void SetDeviceLocationNameWithTimeZoneRule(HLERequestContext& ctx) {
        // O guest manda o nome (0x24) e a regra em buffer A; a regra é
        // lida e descartada (a UTC regula tudo aquilo que as datas
        // deste motor produzem).
        RequestParser rp{ctx};
        const auto pedido = rp.PopRaw<LocationName>();
        local_.assign(pedido.begin(), pedido.end());

        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 8: o binário do guest vira uma regra zeros (a mesma tabela
    // destino do comando 4).
    void ParseTimeZoneBinary(HLERequestContext& ctx) {
        const std::size_t tamanho = ctx.GetWriteBufferSize(0);
        if (tamanho > 0) {
            const std::vector<u8> zeros(tamanho, 0);
            ctx.WriteBuffer(zeros.data(), tamanho);
        }
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 20: o evento do fuso — real, não sinalizado (ver o construtor).
    void GetDeviceLocationNameOperationEventReadableHandle(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &evento_->GetReadableEvent());
    }

    // 100: o calendário deste posix. O guest manda o posix (s64) e a
    // regra num buffer; a UTC regula. Resposta: resultado + CalendarTime
    // (2 palavras) + CalendarAdditionalInfo (6 palavras) = 10.
    void ToCalendarTime(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const s64 posix = rp.PopRaw<s64>();

        ResponseBuilder rb{ctx, 10};
        rb.Push(ResultSuccess);
        rb.PushRaw(estado_->calendario(posix));
        rb.PushRaw(adicional());
    }

    // 101: com a "minha regra" — no fuso único do motor é igual ao 100.
    void ToCalendarTimeWithMyRule(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const s64 posix = rp.PopRaw<s64>();

        ResponseBuilder rb{ctx, 10};
        rb.Push(ResultSuccess);
        rb.PushRaw(estado_->calendario(posix));
        rb.PushRaw(adicional());
    }

    // 201/202: do calendário ao posix. O guest manda o CalendarTime
    // (0x8 = 2 palavras); o posix sai via buffer (OutArray de s64, 1
    // elemento volta com count=1 na palavra normal).
    void ToPosixTimeDe(HLERequestContext& ctx, CalendarTime c) {
        const s64 valor = posix_de_calendario(c);
        ctx.WriteBuffer(&valor, sizeof(valor));

        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(1);
    }

    // Dias desde 1970-01-01 — o algoritmo civil de Howard Hinnant
    // (`days_from_civil`), que devolve já relativo ao epoch.
    static s64 dias_da_epoca(s64 ano, unsigned mes, unsigned dia) {
        ano -= mes <= 2;
        const s64 era = (ano >= 0 ? ano : ano - 399) / 400;
        const s64 yoe = ano - era * 400;                                  // [0, 399]
        const s64 doy = (153 * (mes + (mes > 2 ? -3 : 9)) + 2) / 5 + dia - 1;
        const s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    static s64 posix_de_calendario(const CalendarTime& c) {
        const s64 dias = dias_da_epoca(c.year,
                                       static_cast<unsigned>(c.month),
                                       static_cast<unsigned>(c.day));
        return dias * 86400 + c.hour * 3600 + c.minute * 60 + c.second;
    }

    void ToPosixTime(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto c = rp.PopRaw<CalendarTime>();
        ToPosixTimeDe(ctx, c);
    }

    void ToPosixTimeWithMyRule(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto c = rp.PopRaw<CalendarTime>();
        ToPosixTimeDe(ctx, c);
    }

    std::string local_ = "UTC";
    Kernel::KEvent* evento_ = nullptr;
    EstadoDoRelogio* estado_;
};

// ─────────────────────────────────────────────────────────────────────────────
// `time:u` — o `StaticService` (`psc/time/static.cpp`)
// ─────────────────────────────────────────────────────────────────────────────
//
// A tabela completa do Eden — 19 comandos. O estado (um
// `EstadoDoRelogio`) é do `time:u`, e os relógios e o fuso referem-no
// por ponteiro: as sub-interfaces são criadas a cada pedido (como no
// Eden), mas todas batem **no mesmo estado** — o guest não conta
// instâncias, e o relógio dele é constante.
class TimeU final : public Service::ServiceFramework<TimeU> {
public:
    explicit TimeU(Core::System& system) : ServiceFramework{system, "time:u"} {
        // A tabela é a do Eden (`static.cpp`), à letra.
        static const FunctionInfo functions[] = {
            {0, &TimeU::GetStandardUserSystemClock, "GetStandardUserSystemClock"},
            {1, &TimeU::GetStandardNetworkSystemClock,
             "GetStandardNetworkSystemClock"},
            {2, &TimeU::GetStandardSteadyClock, "GetStandardSteadyClock"},
            {3, &TimeU::GetTimeZoneService, "GetTimeZoneService"},
            {4, &TimeU::GetStandardLocalSystemClock,
             "GetStandardLocalSystemClock"},
            {5, &TimeU::GetEphemeralNetworkSystemClock,
             "GetEphemeralNetworkSystemClock"},
            {20, &TimeU::GetSharedMemoryNativeHandle,
             "GetSharedMemoryNativeHandle"},
            {50, &TimeU::SetStandardSteadyClockInternalOffset,
             "SetStandardSteadyClockInternalOffset"},
            {51, &TimeU::GetStandardSteadyClockRtcValue,
             "GetStandardSteadyClockRtcValue"},
            {100, &TimeU::IsStandardUserSystemClockAutomaticCorrectionEnabled,
             "IsStandardUserSystemClockAutomaticCorrectionEnabled"},
            {101, &TimeU::SetStandardUserSystemClockAutomaticCorrectionEnabled,
             "SetStandardUserSystemClockAutomaticCorrectionEnabled"},
            {102, &TimeU::GetStandardUserSystemClockInitialYear,
             "GetStandardUserSystemClockInitialYear"},
            {200, &TimeU::IsStandardNetworkSystemClockAccuracySufficient,
             "IsStandardNetworkSystemClockAccuracySufficient"},
            {201, &TimeU::GetStandardUserSystemClockAutomaticCorrectionUpdatedTime,
             "GetStandardUserSystemClockAutomaticCorrectionUpdatedTime"},
            {300, &TimeU::CalculateMonotonicSystemClockBaseTimePoint,
             "CalculateMonotonicSystemClockBaseTimePoint"},
            {400, &TimeU::GetClockSnapshot, "GetClockSnapshot"},
            {401, &TimeU::GetClockSnapshotFromSystemClockContext,
             "GetClockSnapshotFromSystemClockContext"},
            {500, &TimeU::CalculateStandardUserSystemClockDifferenceByUser,
             "CalculateStandardUserSystemClockDifferenceByUser"},
            {501, &TimeU::CalculateSpanBetween, "CalculateSpanBetween"},
        };
        RegisterHandlers(functions);

        // O objecto por trás do comando 20. O kernel vendorizado não tem
        // `KSharedMemory` — o que o guest recebe é um **handle real de
        // cópia** para um membro `KEvent`: a forma CMIF à medida (um
        // objecto de cópia, como o `GetEventHandle` d'`applet_oe.cpp`),
        // e o conteúdo do "map" é tratado pelo `svc_map_shared_memory`
        // no-op (processo único, memória já do processo).
        auto& kernel = system.Kernel();
        auto& processo = Kernel::GetCurrentProcess(kernel);
        shm_ = new Kernel::KEvent(kernel);
        shm_->Initialize(kernel, &processo);
    }

    // A peça única de estado: os relógios e o fuso partilham-na.
    EstadoDoRelogio estado_;

private:
    // ── 0..5: as sub-interfaces ──
    //
    // `ResponseBuilder rb{ctx, 2, 0, 1}` — o resultado e **um** objecto
    // a mover (o padrão do `Devolve` d'`applet_oe.cpp`).
    void DevolveRelogioDeSistema(HLERequestContext& ctx, const char* nome) {
        LOG_DEBUG(Service_Time, "{}: devolvido", nome);
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<RelogioDeSistema>(
            ctx, std::make_shared<RelogioDeSistema>(system, &estado_));
    }

    void GetStandardUserSystemClock(HLERequestContext& ctx) {
        DevolveRelogioDeSistema(ctx, "GetStandardUserSystemClock");
    }

    void GetStandardNetworkSystemClock(HLERequestContext& ctx) {
        DevolveRelogioDeSistema(ctx, "GetStandardNetworkSystemClock");
    }

    void GetStandardLocalSystemClock(HLERequestContext& ctx) {
        DevolveRelogioDeSistema(ctx, "GetStandardLocalSystemClock");
    }

    void GetEphemeralNetworkSystemClock(HLERequestContext& ctx) {
        DevolveRelogioDeSistema(ctx, "GetEphemeralNetworkSystemClock");
    }

    void GetStandardSteadyClock(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Time, "GetStandardSteadyClock: devolvido");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<RelogioSteady>(
            ctx, std::make_shared<RelogioSteady>(system, &estado_));
    }

    void GetTimeZoneService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Time, "GetTimeZoneService: devolvido");
        ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<FusoHorario>(
            ctx, std::make_shared<FusoHorario>(system, &estado_));
    }

    // ── 20: a memória partilhada ──
    //
    // No Eden, o `TimeManager` cria um `KSharedMemory` com a estrutura
    // de ticks partilhada com o `nn::time`. O kernel vendorizado **não
    // tem `KSharedMemory`**. Medido (boot_p6d:374-376): a resposta
    // **sem** handle de cópia faz o reader CMIF do guest ir à borda
    // (`nn::ipc` espera 1 objecto de cópia nesta resposta) e o SDK
    // aborta — `SetTerminateResult: modulo=162 desc=1` (o
    // `UserlandAssert` da libnn). Por isso a forma é a do
    // `GetEventHandle` d'`applet_oe.cpp`: **um handle de cópia real**;
    // o `svcMapSharedMemory` do guest cai no no-op do dispatcher, e o
    // conteúdo que ele lê é a memória do próprio processo (zeros).
    void GetSharedMemoryNativeHandle(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Time,
                  "GetSharedMemoryNativeHandle: sem KSharedMemory; "
                  "handle de KEvent (shm_ no-op)");
        ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &shm_->GetReadableEvent());
    }

    // ── 50/51: o offset interno do steady (o Eden devolve
    // NotImplemented; a casa aplica-o no estado partilhado, a medir) ──
    void SetStandardSteadyClockInternalOffset(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const s64 offset_ns = rp.PopRaw<s64>();
        // Em segundos; os nanos que sobram não mudam o segundo no
        // relógio deste motor.
        estado_.interno_s = offset_ns / 1000000000;

        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetStandardSteadyClockRtcValue(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s64>(EstadoDoRelogio::agora());
    }

    // ── 100/101/102: as opções do relógio do utilizador ──
    void IsStandardUserSystemClockAutomaticCorrectionEnabled(
        HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<bool>(estado_.automatico);
    }

    void SetStandardUserSystemClockAutomaticCorrectionEnabled(
        HLERequestContext& ctx) {
        RequestParser rp{ctx};
        estado_.automatico = rp.PopRaw<bool>();

        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetStandardUserSystemClockInitialYear(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        // O ano inicial: o Eden não o implementa; a auto-stub devolveria
        // 0 — um ano morto. O ano do board é coerente com o relógio
        // deste motor.
        const std::time_t agora = static_cast<std::time_t>(EstadoDoRelogio::agora());
        std::tm t{};
        ::gmtime_r(&agora, &t);
        rb.Push<s32>(t.tm_year + 1900);
    }

    // ── 200/201: o relógio de rede ──
    //
    // Sem NTP neste motor, o relógio de rede é o mesmo do utilizador:
    // **suficiente**, e a última correcção é o agora.
    void IsStandardNetworkSystemClockAccuracySufficient(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<bool>(true);
    }

    void GetStandardUserSystemClockAutomaticCorrectionUpdatedTime(
        HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 8};
        rb.Push(ResultSuccess);
        rb.PushRaw(SteadyClockTimePoint{EstadoDoRelogio::agora(),
                                        EstadoDoRelogio::fonte()});
    }

    // ── 300: a base do tempo monótono ──
    //
    // Exatamente o corpo do Eden (`static.cpp`):
    // `out = (ctx.offset + ctx.steady) - agora`, com o `IdMatches` da
    // fonte. Sem bater, o Eden devolve `ResultClockMismatch` — a casa
    // não manda erro: resposta 0 com um log (a medição decide).
    void CalculateMonotonicSystemClockBaseTimePoint(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const auto contexto = rp.PopRaw<SystemClockContext>();

        const s64 agora = EstadoDoRelogio::agora();
        s64 saida = 0;
        if (contexto.steady_time_point.IdMatches(
                SteadyClockTimePoint{0, EstadoDoRelogio::fonte()})) {
            saida =
                (contexto.offset + contexto.steady_time_point.time_point) - agora;
        } else {
            LOG_WARNING(Service_Time,
                        "CalculateMonotonicSystemClockBaseTimePoint: fonte "
                        "nao bate — resposta 0");
        }

        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s64>(saida);
    }

    // ── 400/401/500/501: os snapshots ──
    //
    // O snapshot é uma `OutLargeData` — escreve-se no buffer do guest
    // (`WriteBuffer`), não por palavras. A construção é a do Eden
    // (`GetClockSnapshotImpl`), com os relógios deste motor e o fuso
    // UTC: contextos vivos, o posix pela fórmula do Eden, os
    // calendários por `gmtime` — e o campo `type` do pedido.
    static LocationName nome_utc() {
        LocationName retencao{};
        std::memcpy(retencao.data(), "UTC", 3);
        return retencao;
    }

    void GetClockSnapshotImpl(HLERequestContext& ctx, TimeType tipo,
                              const SystemClockContext* user_ctx,
                              const SystemClockContext* network_ctx) {
        ClockSnapshot snap{};

        snap.user_context = user_ctx ? *user_ctx : estado_.contexto();
        snap.network_context = network_ctx ? *network_ctx : estado_.contexto();

        snap.steady_clock_time_point =
            SteadyClockTimePoint{EstadoDoRelogio::agora(),
                                 EstadoDoRelogio::fonte()};
        snap.is_automatic_correction_enabled = estado_.automatico;
        snap.location_name = nome_utc();
        snap.type = tipo;

        // O posix do Eden: `ctx.offset + ctx.steady` (com IdMatches).
        // Sem bater: 0 — a regra sem erro.
        auto posix_de = [](const SystemClockContext& c) -> s64 {
            if (c.steady_time_point.IdMatches(
                    SteadyClockTimePoint{0, EstadoDoRelogio::fonte()}))
                return c.offset + c.steady_time_point.time_point;
            return 0;
        };
        snap.user_time = posix_de(snap.user_context);
        snap.network_time = posix_de(snap.network_context);

        snap.user_calendar_time = estado_.calendario(snap.user_time);
        snap.network_calendar_time = estado_.calendario(snap.network_time);
        snap.user_calendar_additional_time = adicional();
        snap.network_calendar_additional_time = adicional();
        snap.unk_CE = 0;

        ctx.WriteBuffer(&snap, sizeof(snap));

        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetClockSnapshot(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const u32 tipo_bruto = rp.PopRaw<u32>();
        GetClockSnapshotImpl(ctx, static_cast<TimeType>(tipo_bruto & 0xFF),
                             nullptr, nullptr);
    }

    void GetClockSnapshotFromSystemClockContext(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const u32 tipo_bruto = rp.PopRaw<u32>();
        const auto user_ctx = rp.PopRaw<SystemClockContext>();
        const auto network_ctx = rp.PopRaw<SystemClockContext>();
        GetClockSnapshotImpl(ctx, static_cast<TimeType>(tipo_bruto & 0xFF),
                             &user_ctx, &network_ctx);
    }

    // 500: a diferença entre dois snapshots — o corpo do Eden, à
    // letra. Os snapshots chegam em buffers (A/B); lêem-se os dois.
    void CalculateStandardUserSystemClockDifferenceByUser(HLERequestContext& ctx) {
        const auto a_bytes = ctx.ReadBuffer(0);
        const auto b_bytes = ctx.ReadBuffer(1);
        ClockSnapshot a{}, b{};
        if (a_bytes.size() >= sizeof(a))
            std::memcpy(&a, a_bytes.data(), sizeof(a));
        if (b_bytes.size() >= sizeof(b))
            std::memcpy(&b, b_bytes.data(), sizeof(b));

        s64 saida = 0;
        if (a.user_context == b.user_context ||
            !a.user_context.steady_time_point.IdMatches(
                b.user_context.steady_time_point)) {
            saida = 0;
        } else {
            // Sem correcção automática (ou com ela — no Eden os dois
            // ramos caem na mesma conta com a regra da fonte): s → ns.
            saida = (b.user_context.offset - a.user_context.offset) * 1000000000;
        }

        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s64>(saida);
    }

    // 501: a distância entre dois pontos-firme — o corpo do Eden, à
    // letra: se as fontes baterem, é a diferença dos pontos; senão, com
    // o network time de ambos.
    void CalculateSpanBetween(HLERequestContext& ctx) {
        const auto a_bytes = ctx.ReadBuffer(0);
        const auto b_bytes = ctx.ReadBuffer(1);
        ClockSnapshot a{}, b{};
        if (a_bytes.size() >= sizeof(a))
            std::memcpy(&a, a_bytes.data(), sizeof(a));
        if (b_bytes.size() >= sizeof(b))
            std::memcpy(&b, b_bytes.data(), sizeof(b));

        s64 saida = 0;
        if (a.steady_clock_time_point.IdMatches(b.steady_clock_time_point)) {
            saida = (b.steady_clock_time_point.time_point -
                     a.steady_clock_time_point.time_point) * 1000000000;
        } else if (a.network_time != 0 && b.network_time != 0) {
            saida = (b.network_time - a.network_time) * 1000000000;
        } else {
            LOG_WARNING(Service_Time, "CalculateSpanBetween: fontes fora — resposta 0");
        }

        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s64>(saida);
    }

    // O stand-in do `KSharedMemory` (a nota do construtor explica-o).
    Kernel::KEvent* shm_ = nullptr;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// O registo
// ─────────────────────────────────────────────────────────────────────────────

// Regista o `time:u` no `sm:` — o mesmo caminho do `appletOE`, do `apm`
// e do `aoc:u`: um `ServerManager` próprio que fica no meta-registo do
// pump.
void registar_time_u(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("time:u", std::make_shared<TimeU>(system));
    Service::ServerManager::RunServer(std::move(server_manager));
}

} // namespace g610::services
