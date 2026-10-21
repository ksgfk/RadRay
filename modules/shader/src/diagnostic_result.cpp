#include <radray/shader/diagnostic_result.h>

namespace radray::shader {

void AppendDiagnostic(string& diagnostics, std::string_view message) {
    if (message.empty()) return;
    const size_t previousSize = diagnostics.size();
    const bool needsSeparator = previousSize != 0 && diagnostics.back() != '\n';
    diagnostics.append(message);
    if (needsSeparator) diagnostics.insert(previousSize, 1, '\n');
}

}  // namespace radray::shader
