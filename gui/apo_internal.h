#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace apo::detail {

bool RegReadString(HKEY root, const std::wstring& sub, const wchar_t* name,
                   std::wstring& out);
bool RegReadMultiString(HKEY root, const std::wstring& sub,
                        const wchar_t* name, std::vector<std::wstring>& out);
bool RegReadDword(HKEY root, const std::wstring& sub, const wchar_t* name,
                  DWORD& out);
bool RegKeyExists(HKEY root, const std::wstring& sub);
bool RegWriteString(HKEY root, const std::wstring& sub, const wchar_t* name,
                    const std::wstring& value);
bool RegWriteMultiString(HKEY root, const std::wstring& sub,
                         const wchar_t* name,
                         const std::vector<std::wstring>& values);
bool RegWriteDword(HKEY root, const std::wstring& sub, const wchar_t* name,
                   DWORD value);
bool RegDeleteValue(HKEY root, const std::wstring& sub, const wchar_t* name);
void DeleteKeyTree(HKEY root, const std::wstring& sub);

std::wstring FxSub(const std::wstring& id);
std::wstring SafeId(const std::wstring& id);
std::wstring System32Path(const wchar_t* name);
std::wstring Quote(const std::wstring& s);
std::wstring Timestamp();

std::wstring ReadTextFile(const std::wstring& path);
bool WriteTextFile(const std::wstring& path, const std::wstring& text);
bool RunProcess(const std::wstring& command, std::wstring& output,
                DWORD& exitCode);
bool GrantLocalServiceModify(const std::wstring& dir, std::wstring& log);
bool ExtractRegString(const std::wstring& text, const std::wstring& name,
                      std::wstring& value);
bool ExtractRegMultiString(const std::wstring& text, const std::wstring& name,
                           std::vector<std::wstring>& values);

} // namespace apo::detail
