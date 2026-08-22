/**
 * \file src/system.cc
 * \ingroup system
 *
 * Provide a high-level interface to system-related features, like filesystem manipulations.
 *
 * Ideally, all OS-specific features should be grouped here.
 *
 * This modules shoumd not depend on any other opustags module.
 */

#include <opustags.h>

#include <errno.h>
#include <fstream>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <share.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "win32_compat.h"
#else
#include <sys/wait.h>
#endif

#ifdef _WIN32
// getdelim() and mkstemps() are POSIX/BSD extensions the MSVCRT/UCRT does
// not provide. HAVE_GETDELIM / HAVE_MKSTEMPS are defined by CMake via
// check_function_exists() when the target runtime already has them, so
// these definitions only compile in when actually needed, rather than
// relying on a macro-name guard that can't detect a real function.
//
// These are scoped to Windows only and match the real functions' exact
// signatures, so they're drop-in replacements: ot::read_comments() in
// cli.cc calls getdelim() exactly the same way on every platform, and
// this file is the only place that changes.

#ifndef HAVE_GETDELIM
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream)
{
	if (lineptr == nullptr || n == nullptr || stream == nullptr) {
		errno = EINVAL;
		return -1;
	}

	// Accumulate into a std::string, which grows itself, rather than
	// manually doubling a malloc'd buffer by hand. We still hand the
	// result back through the caller's malloc'd buffer at the end, since
	// that's the contract getdelim() callers (and free()) expect.
	std::string buffer;
	int c;
	while ((c = fgetc(stream)) != EOF) {
		buffer.push_back(static_cast<char>(c));
		if (c == delim)
			break;
	}
	if (buffer.empty())
		return -1; // Nothing left to read.

   size_t needed = buffer.size() + 1; // +1 for the null terminator.
   if (*lineptr == nullptr || *n < needed) {
   	char* newbuf = static_cast<char*>(realloc(*lineptr, needed));
   	if (newbuf == nullptr) {
   		errno = ENOMEM;
   		return -1;
   	}
   	*lineptr = newbuf;
   	*n = needed;
   }
	memcpy(*lineptr, buffer.data(), buffer.size());
	(*lineptr)[buffer.size()] = '\0';
	return static_cast<ssize_t>(buffer.size());
}
#endif // HAVE_GETDELIM

#ifndef HAVE_MKSTEMPS
int mkstemps(char* tmpl, int suffixlen)
{
	size_t len = strlen(tmpl);
	if (suffixlen < 0 || len < static_cast<size_t>(6 + suffixlen))
		return -1;
	size_t placeholder_end = len - static_cast<size_t>(suffixlen);

	// _mktemp_s requires the "XXXXXX" placeholder to be the last six
	// characters of the string it operates on, but mkstemps' templates
	// have a suffix after the placeholder (e.g. ".part"), which _mktemp_s
	// does not support directly. Work around this by temporarily
	// truncating the suffix off, letting _mktemp_s fill in the XXXXXX
	// portion with its own (better than rand()) uniqueness algorithm,
	// then restoring the suffix.

	// Save the suffix using std::string (no fixed size limit)
	std::string saved_suffix(tmpl + placeholder_end, suffixlen);
	tmpl[placeholder_end] = '\0';

	errno_t err = _mktemp_s(tmpl, placeholder_end + 1);

	// Restore the suffix
	memcpy(tmpl + placeholder_end, saved_suffix.data(), suffixlen);
	tmpl[len] = '\0';

	if (err != 0) {
		errno = err;
		return -1;
	}

	// _mktemp_s only picks a name, it does not create or open the file, so
	// the O_CREAT|O_EXCL open below is what actually gives us mkstemps'
	// atomicity guarantee (the name didn't exist and now it's ours).

	int fd;
	if (_sopen_s(&fd, tmpl, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _SH_DENYNO,
	             _S_IREAD | _S_IWRITE) != 0)
		return -1;
	return fd;
}
#endif // HAVE_MKSTEMPS
#endif // _WIN32

void ot::close_file(FILE* file)
{
	fclose(file);
}

void ot::partial_file::open(const char* destination)
{
	final_name = destination;
	temporary_name = final_name + ".XXXXXX.part";
	int fd = mkstemps(const_cast<char*>(temporary_name.data()), 5);
	if (fd == -1)
		throw status {st::standard_error,
		              "Could not create a partial file for '" + final_name + "': " +
		              strerror(errno)};
	file = fdopen(fd, "w");
	if (file == nullptr)
		throw status {st::standard_error,
		              "Could not get the partial file handle to '" + temporary_name + "': " +
		              strerror(errno)};
}

#ifndef _WIN32
static mode_t get_umask()
{
	// libc doesn’t seem to provide a way to get umask without changing it, so we need this workaround.
	// https://www.gnu.org/software/libc/manual/html_node/Setting-Permissions.html
	mode_t mask = umask(0);
	umask(mask);
	return mask;
}

/**
 * Try reproducing the file permissions of file `source` onto file `dest`. If
 * this fails for whatever reason, print a warning and leave the current
 * permissions. When the source doesn’t exist, use the default file creation
 * permissions according to umask.
 */
static void copy_permissions(const char* source, const char* dest)
{
	mode_t target_mode;
	struct stat source_stat;
	if (stat(source, &source_stat) == 0) {
		// We could technically preserve a bit more than that but who
		// would ever need S_ISUID and friends on an Opus file?
		target_mode = source_stat.st_mode & 0777;
	} else if (errno == ENOENT) {
		target_mode = 0666 & ~get_umask();
	} else {
		fprintf(stderr, "warning: Could not read mode of %s: %s\n", source, strerror(errno));
		return;
	}
	if (chmod(dest, target_mode) == -1)
		fprintf(stderr, "warning: Could not set mode of %s: %s\n", dest, strerror(errno));
}
#endif

void ot::partial_file::commit()
{
	if (file == nullptr)
		return;
	file.reset();
#ifndef _WIN32
   // Windows does not use Unix-style file modes; the temporary file already has the correct
   // default permissions. On Unix, we copy the original file's permissions.
	copy_permissions(final_name.c_str(), temporary_name.c_str());
#endif

#ifdef _WIN32
	// Windows rename() refuses to overwrite an existing file
	if (remove(final_name.c_str()) != 0 && errno != ENOENT) {
		throw status {st::standard_error,
		              "Could not remove original file '" + final_name + "': " +
		              strerror(errno) + "."};
	}
#endif
	if (rename(temporary_name.c_str(), final_name.c_str()) == -1)
		throw status {st::standard_error,
		              "Could not move the result file '" + temporary_name + "' to '" +
		              final_name + "': " + strerror(errno) + "."};
}

void ot::partial_file::abort()
{
	if (file == nullptr)
		return;
	file.reset();
	remove(temporary_name.c_str());
}

/**
 * Determine the file size, in bytes, of the given file. Return -1 on for streams.
 */
static long get_file_size(FILE* f)
{
	if (fseek(f, 0L, SEEK_END) != 0) {
		clearerr(f); // Recover.
		return -1;
	}
	long file_size = ftell(f);
	rewind(f);
	return file_size;
}

ot::byte_string ot::slurp_binary_file(const char* filename)
{
	file f = strcmp(filename, "-") == 0 ? freopen(nullptr, "rb", stdin)
	                                    : fopen(filename, "rb");
	if (f == nullptr)
		throw status { st::standard_error,
		               "Could not open '"s + filename + "': " + strerror(errno) + "." };

	byte_string content;
	long file_size = get_file_size(f.get());
	if (file_size < 0) {
		// Read the input stream block by block and resize the output byte string as needed.
		char buffer[4096];
		while (!feof(f.get())) {
			size_t read_len = fread(buffer, 1, sizeof(buffer), f.get());
			content.append(buffer, read_len);
			if (ferror(f.get()))
				throw status { st::standard_error,
					       "Could not read '"s + filename + "': " + strerror(errno) + "." };
		}
	} else {
		// Lucky! We know the file size, so let’s slurp it at once.
		content.resize(file_size);
		if (fread(content.data(), 1, file_size, f.get()) < size_t(file_size))
			throw status { st::standard_error,
				       "Could not read '"s + filename + "': " + strerror(errno) + "." };
	}

	return content;
}

/** C++ wrapper for iconv. */
class encoding_converter {
public:
	/**
	 * Allocate the iconv conversion state, initializing the given source and destination
	 * character encodings. If it's okay to have some information lost, make sure `to` ends with
	 * "//TRANSLIT", otherwise the conversion will fail when a character cannot be represented
	 * in the target encoding. See the documentation of iconv_open for details.
	 */
	encoding_converter(const char* from, const char* to);
	~encoding_converter();
	/**
	 * Convert text using iconv. If the input sequence is invalid, return #st::badly_encoded and
	 * abort the processing, leaving out in an undefined state.
	 */
	template<class InChar, class OutChar>
	std::basic_string<OutChar> convert(std::basic_string_view<InChar>);
private:
	iconv_t cd; /**< conversion descriptor */
};

encoding_converter::encoding_converter(const char* from, const char* to)
{
	cd = iconv_open(to, from);
	if (cd == (iconv_t) -1)
		throw std::bad_alloc();
}

encoding_converter::~encoding_converter()
{
	iconv_close(cd);
}

template<class InChar, class OutChar>
std::basic_string<OutChar> encoding_converter::convert(std::basic_string_view<InChar> in)
{
	iconv(cd, nullptr, nullptr, nullptr, nullptr);
	std::basic_string<OutChar> out;
	out.reserve(in.size());
	const char* in_data = reinterpret_cast<const char*>(in.data());
	char* in_cursor = const_cast<char*>(in_data);
	size_t in_left = in.size();
	constexpr size_t chunk_size = 1024;
	char chunk[chunk_size];
	for (;;) {
		char *out_cursor = chunk;
		size_t out_left = chunk_size;
		size_t rc = iconv(cd, &in_cursor, &in_left, &out_cursor, &out_left);

		if (rc == (size_t) -1 && errno == E2BIG) {
			// Loop normally.
		} else if (rc == (size_t) -1) {
			throw ot::status {ot::st::badly_encoded, strerror(errno) + "."s};
		} else if (rc != 0) {
			throw ot::status {ot::st::badly_encoded,
			                 "Some characters could not be converted into the target encoding."};
		}

		out.append(reinterpret_cast<OutChar*>(chunk), out_cursor - chunk);
		if (in_cursor == nullptr)
			break;
		else if (in_left == 0)
			in_cursor = nullptr;
	}
	return out;
}

std::u8string ot::encode_utf8(std::string_view in)
{
	static encoding_converter to_utf8_cvt("", "UTF-8");
	return to_utf8_cvt.convert<char, char8_t>(in);
}

std::string ot::decode_utf8(std::u8string_view in)
{
	static encoding_converter from_utf8_cvt("UTF-8", "");
	return from_utf8_cvt.convert<char8_t, char>(in);
}

std::string ot::shell_escape(std::string_view word)
{
#ifdef _WIN32
	if (!word.empty() && word.find_first_of(" \t\n\v\"") == std::string_view::npos)
		return std::string(word);

	std::string escaped = "\"";
	for (auto it = word.begin();; ++it) {
		unsigned backslashes = 0;
		while (it != word.end() && *it == '\\') {
			++backslashes;
			++it;
		}
		if (it == word.end()) {
			// Escape all backslashes, since they're followed by the closing quote.
			escaped.append(backslashes * 2, '\\');
			break;
		} else if (*it == '"') {
			// Escape all backslashes and the quote itself.
			escaped.append(backslashes * 2 + 1, '\\');
			escaped += '"';
		} else {
			// Backslashes aren't special here.
			escaped.append(backslashes, '\\');
			escaped += *it;
		}
	}
	escaped += '"';
	return escaped;
#else
	std::string escaped_word;
	// Pre-allocate the result, assuming most of the time enclosing it in single quotes is enough.
	escaped_word.reserve(2 + word.size());

	escaped_word += '\'';
	for (char c : word) {
		if (c == '\'')
			escaped_word += "'\\''";
		else if (c == '!')
			escaped_word += "'\\!'";
		else
			escaped_word += c;
	}
	escaped_word += '\'';

	return escaped_word;
#endif
}

#ifdef _WIN32
/**
 * Resolve a path to an absolute one using the Win32 API directly, rather
 * than std::filesystem::absolute(). libstdc++'s filesystem implementation
 * on MinGW does a narrow<->wide character-set conversion internally and
 * throws filesystem_error on byte sequences it considers "illegal" under
 * the current locale/codepage -- this was observed to crash on ordinary
 * accented Latin characters.
 *
 * GetFullPathNameA operates on raw bytes with no encoding validation at
 * all, so it can't fail this way.
 */
static std::string win32_absolute_path(std::string_view path)
{
	std::string input(path);
	char buffer[MAX_PATH];
	DWORD len = GetFullPathNameA(input.c_str(), MAX_PATH, buffer, nullptr);
	if (len == 0 || len >= MAX_PATH)
		return input; // Fall back to the original path if resolution fails.
	return std::string(buffer, len);
}
#endif

void ot::run_editor(std::string_view editor, std::string_view path)
{
	// Always pass an absolute path to the editor. This is the surest way to
	// avoid the editor misinterpreting the path as an option if it happens
	// to start with '-' -- more reliable than "--", which not every editor
	// respects the same way (observed difference in behavior between
	// Notepad and Neovim on Windows).
#ifdef _WIN32
	std::string abs_path = win32_absolute_path(path);
	std::string command = std::string(editor) + " " + shell_escape(abs_path);
#else
	std::string command = std::string(editor) + " -- " + shell_escape(path);
#endif

	int status = system(command.c_str());

#ifdef _WIN32
	// On Windows, system() returns the exit code directly (or -1 on error)
	if (status == -1)
		throw ot::status {st::standard_error, "system() error: "s + strerror(errno)};
	else if (status != 0)
		throw ot::status {st::child_process_failed,
		                  "Child process exited with " + std::to_string(status)};
#else
	if (status == -1)
		throw ot::status {st::standard_error, "waitpid error: "s + strerror(errno)};
	else if (!WIFEXITED(status))
		throw ot::status {st::child_process_failed,
		                 "Child process did not terminate normally: "s + strerror(errno)};
	else if (WEXITSTATUS(status) != 0)
		throw ot::status {st::child_process_failed,
		                  "Child process exited with " + std::to_string(WEXITSTATUS(status))};
#endif
}

timespec ot::get_file_timestamp(const char* path)
{
	timespec mtime;
	struct stat st;
	if (stat(path, &st) == -1)
		throw status {st::standard_error, path + ": stat error: "s + strerror(errno)};
#if defined(HAVE_STAT_ST_MTIM)
	mtime = st.st_mtim;
#elif defined(HAVE_STAT_ST_MTIMESPEC)
	mtime = st.st_mtimespec;
#elif defined(_WIN32)
	mtime.tv_sec = st.st_mtime;
	mtime.tv_nsec = 0;
#else
	mtime.tv_sec = st.st_mtime;
	mtime.tv_nsec = st.st_mtimensec;
#endif
	return mtime;
}
