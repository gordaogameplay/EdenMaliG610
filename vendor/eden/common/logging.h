// SPDX-FileCopyrightText: 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// O logger da casa.
//
// ## Porque este ficheiro é uma ponte
//
// O upstream partiu o logger em **três** ficheiros ✓:
//
//     common/logging/formatter.h   os `fmt::formatter` dos enums e dos
//                                  `SwapStructT` ✓
//     common/logging/types.h       `Common::Log::Level` e `Common::Log::Class` ✓
//     common/logging/log.h         `FmtLogMessage` e as macros `LOG_*` ✓
//
// A casa tinha **um** `common/logging.h` ✗ — o antigo, do Citra ✗ — com
// **as mesmas três coisas** dentro ✗. Quando o `video_core` do upstream
// entrou na casa ✗, as duas definições coexistiram ✗ e o compilador
// queixou-se ✗:
//
//     formatter.h:17: error: redefinition of 'struct fmt::v11::formatter<T, ...>'
//     types.h:11:    error: multiple definition of 'enum class Common::Log::Level'
//     log.h:25:      error: redefinition of 'void Common::Log::FmtLogMessage(...)'
//
// **Nenhuma das duas cópias era a "certa"** ✗ — a da casa era a antiga ✗ e
// a do upstream era a nova ✗; o que faltava era **uma só** ✓.
//
// ## A lista de classes é a mesma
//
// Medido ✓: as 99 classes do `log_classes.inc` da casa e as 99 do
// `types.h` do upstream são **as mesmas** ✓ — nenhuma de um lado que não
// exista do outro ✓. Por isso esta ponte pode ficar com a do upstream ✓
// sem perder nada ✓.
//
// (O `log_classes.inc` da casa fica aqui ✓ — inerte ✗ — porque é a fonte
// de verdade da lista ✓ e porque ninguém o deve apagar ✗.)
//
// ## O que fica aqui
//
// A API que **é da casa** ✓: o `Filter` ✓, o `Entry` ✓, o
// `Initialize`/`Start`/`Stop` ✓ e as funções de impressão ✓.
//
// O upstream implementa-as em `common/logging/backend.cpp` ✗ — que a casa
// **não** compila ✗. A implementação é `src/eden_log.cpp` ✓ (ver o
// ficheiro ✗: são dois símbolos ✓ — `FmtLogMessageImpl` ✓ e `Stop` ✓).

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>

// O upstream ✓ — traz os `formatter` ✓, os tipos ✓, o `FmtLogMessage` ✓ e
// as macros `LOG_*` ✓.
//
// **O caminho é o do upstream** ✓ (`common/logging/log.h` ✓) e não o da
// casa ✗ (`vendor/eden/common/logging.h` ✗) — senão isto includia-se a
// si próprio ✗.
#include "common/logging/log.h"

namespace Common::Log {

/// Implements a log message filter which allows different log classes to have different minimum
/// severity levels. The filter can be changed at runtime and can be parsed from a string to allow
/// editing via the interface or loading from a configuration file.
struct Filter {
    /// Initializes the filter with all classes having `default_level` as the minimum level.
    explicit Filter(Level level = Level::Info) {
        class_levels.fill(level);
    }
    /// Sets the minimum level of `log_class` (and not of its subclasses) to `level`.
    void SetClassLevel(Class log_class, Level level) {
        class_levels[std::size_t(log_class)] = level;
    }
    /// Parses a filter string and applies it to this filter.
    ///
    /// A filter string consists of a space-separated list of filter rules, each of the format
    /// `<class>:<level>`. `<class>` is a log class name, with subclasses separated using periods.
    /// `*` is allowed as a class name and will reset all filters to the specified level. `<level>`
    /// a severity level name which will be set as the minimum logging level of the matched classes.
    /// Rules are applied left to right, with each rule overriding previous ones in the sequence.
    /// A few examples of filter rules:
    ///  - `*:Info` -- Resets the filter to show all messages at `Info` level and above.
    ///  - `Service:Info` -- Sets the minimum level of `Service` to `Info`.
    ///  - `Service.FS:Trace` -- Sets the minimum level of `Service.FS` to `Trace`.
    void ParseFilterString(std::string_view filter_view);
    /// Matches class/level combination against the filter, returning true if it passed.
    [[nodiscard]] bool CheckMessage(Class log_class, Level level) const {
        return u8(level) >= u8(class_levels[std::size_t(log_class)]);
    }
    /// Returns true if any logging classes are set to debug
    [[nodiscard]] bool IsDebug() const {
        return std::any_of(class_levels.begin(), class_levels.end(), [](const Level& l) {
            return u8(l) <= u8(Level::Debug);
        });
    }
    std::array<Level, std::size_t(Class::Count)> class_levels;
};

/// Initializes the logging system. This should be the first thing called in main.
void Initialize();

void Start();

/// Explicitly stops the logger thread and flushes the buffers
void Stop();

/// The global filter will prevent any messages from even being processed if they are filtered.
void SetGlobalFilter(const Filter& filter);
void SetColorConsoleBackendEnabled(bool enabled);

/// @brief A log entry. Log entries are store in a structured format to permit more varied output
/// formatting on different frontends, as well as facilitating filtering and aggregation.
struct Entry {
    std::string message;
    std::chrono::microseconds timestamp;
    Class log_class{};
    Level log_level{};
    const char* filename = nullptr;
    const char* function = nullptr;
    unsigned int line_num = 0;
};

/// Formats a log entry into the provided text buffer.
std::string FormatLogMessage(const Entry& entry) noexcept;

/// Prints the same message as `PrintMessage`, but colored according to the severity level.
void PrintColoredMessage(const Entry& entry) noexcept;

/// Formats and prints the log entry to the android logcat.
void PrintMessageToLogcat(const Entry& entry) noexcept;

} // namespace Common::Log
