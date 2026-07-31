#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <share.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

inline constexpr WORD GPrismConsoleColorDefault = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
inline constexpr WORD GPrismConsoleColorWarning = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
inline constexpr WORD GPrismConsoleColorError = FOREGROUND_RED | FOREGROUND_INTENSITY;

inline WORD FindConsoleLineColorA(const char* Text, size_t Length)
{
	for (size_t i = 0; i < Length; ++i)
	{
		if (i + 9 <= Length && (std::strncmp(Text + i, ": Error: ", 9) == 0 || std::strncmp(Text + i, ": Fatal: ", 9) == 0))
			return GPrismConsoleColorError;
		if (i + 11 <= Length && std::strncmp(Text + i, ": Warning: ", 11) == 0)
			return GPrismConsoleColorWarning;
	}
	return GPrismConsoleColorDefault;
}

inline HANDLE GetPrismConsoleHandle()
{
	static HANDLE Handle = []() -> HANDLE
		{
			const HANDLE ConOut = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (ConOut != INVALID_HANDLE_VALUE)
				return ConOut;

			const HANDLE StdOut = GetStdHandle(STD_OUTPUT_HANDLE);
			return StdOut == INVALID_HANDLE_VALUE ? nullptr : StdOut;
		}();
	return Handle;
}

inline FILE* GetPrismLogFile()
{
	static FILE* LogFile = _fsopen("Prism.log", "w", _SH_DENYWR);
	return LogFile;
}

inline void WriteFileTimestamp(FILE* File)
{
	SYSTEMTIME Time;
	GetLocalTime(&Time);
	std::fprintf(File, "[%02d:%02d:%02d.%03d] ", Time.wHour, Time.wMinute, Time.wSecond, Time.wMilliseconds);
}

inline std::string FormatPrismLine(const char* Format, va_list Args)
{
	va_list SizeArgs;
	va_copy(SizeArgs, Args);
	const int Length = std::vsnprintf(nullptr, 0, Format, SizeArgs);
	va_end(SizeArgs);

	if (Length <= 0)
		return std::string();

	std::string Result(static_cast<size_t>(Length), '\0');
	std::vsnprintf(Result.data(), Result.size() + 1, Format, Args);
	return Result;
}

inline void PrismLogV(const char* Format, va_list Args)
{
	va_list ArgsCopy;
	va_copy(ArgsCopy, Args);
	std::vprintf(Format, Args);

	if (FILE* File = GetPrismLogFile())
	{
		std::vfprintf(File, Format, ArgsCopy);
		std::fflush(File);
	}
	va_end(ArgsCopy);
}

inline void PrismLogRaw(const char* Format, ...)
{
	if (FILE* File = GetPrismLogFile())
		WriteFileTimestamp(File);

	va_list Args;
	va_start(Args, Format);
	PrismLogV(Format, Args);
	va_end(Args);
}

namespace PrismLogDetail
{
	template <typename T>
	concept CHasCStr = requires(const T & V) { static_cast<const char*>(V.c_str()); };

	template <typename T>
	concept CHasWideDeref = requires(const T & V) { static_cast<const wchar_t*>(*V); };

	template <typename T>
	concept CHasToStringMember = requires(const T & V) { V.ToString(); };

	inline const char* Narrow(std::vector<std::string>& Storage, const wchar_t* Wide)
	{
		std::string Result;
		if (Wide)
			for (const wchar_t* P = Wide; *P; ++P)
				Result.push_back(static_cast<char>(*P));
		Storage.push_back(std::move(Result));
		return Storage.back().c_str();
	}

	template <typename T>
	inline decltype(auto) Convert(std::vector<std::string>& Storage, T&& Value)
	{
		using DecayedT = std::decay_t<T>;
		if constexpr (CHasCStr<DecayedT>)
			return Value.c_str();
		else if constexpr (CHasWideDeref<DecayedT>)
			return Narrow(Storage, *Value);
		else if constexpr (CHasToStringMember<DecayedT>)
			return Convert(Storage, Value.ToString());
		else
			return std::forward<T>(Value);
	}
}

template <typename... Args>
inline void PrismLog(const char* Format, Args&&... InArgs)
{
	std::vector<std::string> Storage;
	Storage.reserve(sizeof...(Args));
	PrismLogRaw(Format, PrismLogDetail::Convert(Storage, std::forward<Args>(InArgs))...);
}

struct FMsg
{
	template <typename... Args>
	static void Logf(const char* Category, const char* Verbosity, const char* Format, Args&&... InArgs)
	{
		std::vector<std::string> Storage;
		Storage.reserve(sizeof...(Args));
		LogfImpl(Category, Verbosity, Format, PrismLogDetail::Convert(Storage, std::forward<Args>(InArgs))...);
	}

private:
	static void LogfImpl(const char* Category, const char* Verbosity, const char* Format, ...)
	{
		va_list Args;
		va_start(Args, Format);
		const std::string Body = FormatPrismLine(Format, Args);
		va_end(Args);

		std::string Line;
		Line.reserve(std::strlen(Category) + std::strlen(Verbosity) + Body.size() + 6);
		Line += Category;
		Line += ": ";
		Line += Verbosity;
		Line += ": ";
		Line += Body;
		Line += "\n";

		std::fputs(Line.c_str(), stdout);
		std::fflush(stdout);

		if (FILE* File = GetPrismLogFile())
		{
			WriteFileTimestamp(File);
			std::fputs(Line.c_str(), File);
			std::fflush(File);
		}
	}
};

#define UE_LOG(CategoryName, Verbosity, Format, ...) \
	::FMsg::Logf(#CategoryName, #Verbosity, Format, ##__VA_ARGS__)

#ifdef _DEBUG
#define UE_LOG_DEBUG(CategoryName, Verbosity, Format, ...) \
	::FMsg::Logf(#CategoryName, #Verbosity, Format, ##__VA_ARGS__)
#else
#define UE_LOG_DEBUG(CategoryName, Verbosity, Format, ...) do {} while (0)
#endif
