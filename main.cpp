#include "main.h"

#include <algorithm>
#include <filesystem>

struct Error {
	const std::string message;
	const std::string filePath;
	const std::string function;
	const std::string source;
	const std::string line;
};

static bool isProgressBarActive = false;
static uint32_t filesSkipped = 0;

static struct {
	bool showHelp = false;
	bool silentAssertions = false;
	bool forceOverwrite = false;
	bool ignoreDebugInfo = false;
	bool minimizeDiffs = false;
	bool unrestrictedAscii = false;
	std::string inputPath;
	std::string outputPath;
	std::string extensionFilter;
} arguments;

static std::string string_to_lowercase(const std::string& string) {
	std::string lowercaseString = string;

	for (uint32_t i = lowercaseString.size(); i--;) {
		if (lowercaseString[i] < 'A' || lowercaseString[i] > 'Z') continue;
		lowercaseString[i] += 'a' - 'A';
	}

	return lowercaseString;
}

// 输出文件名：去掉 ".bytes" 包装后缀，保证最终只保留一个 ".lua"。
//   xxx.lua.bytes -> xxx.lua
//   yyy.bytes     -> yyy.lua
//   foo.lua       -> foo.lua
//   foo           -> foo.lua
static std::string output_name_for(const std::filesystem::path& file) {
	std::string name = file.filename().string();

	if (name.size() > 6 && name.ends_with(".bytes")) {
		name.resize(name.size() - 6);
	}

	if (!name.ends_with(".lua")) name += ".lua";

	return name;
}

static bool decompile_file(
	const std::filesystem::path& inputFile,
	const std::filesystem::path& outputFile
) {
	try {
		Bytecode bytecode(inputFile.string());
		Ast ast(bytecode, arguments.ignoreDebugInfo, arguments.minimizeDiffs);
		Lua lua(
			bytecode, ast, outputFile.string(),
			arguments.forceOverwrite, arguments.minimizeDiffs,
			arguments.unrestrictedAscii
		);

		print("--------------------\nInput file: " + inputFile.string() + "\nReading bytecode...");
		bytecode();
		print("Building ast...");
		ast();
		print("Writing lua source...");
		lua();
		print("Output file: " + outputFile.string());
	} catch (const Error& error) {
		erase_progress_bar();

		if (arguments.silentAssertions) {
			print("\nError running " + error.function + "\nSource: " + error.source + ":" + error.line + "\n\n" + error.message);
		} else {
			print("\nError running " + error.function + "\nSource: " + error.source + ":" + error.line
				+ "\n\nFile: " + error.filePath + "\n\n" + error.message);
		}

		filesSkipped++;
		return false;
	} catch (...) {
		print("Unknown exception\n\nFile: " + inputFile.string());
		throw;
	}

	return true;
}

static char* parse_arguments(const int& argc, char* const* argv) {
	if (argc < 2) return nullptr;
	arguments.inputPath = argv[1];

	bool isInputPathSet = true;

	if (arguments.inputPath.size() && arguments.inputPath.front() == '-') {
		arguments.inputPath.clear();
		isInputPathSet = false;
	}

	std::string argument;

	for (uint32_t i = isInputPathSet ? 2 : 1; i < argc; i++) {
		argument = argv[i];

		if (argument.size() >= 2 && argument.front() == '-') {
			if (argument[1] == '-') {
				argument = argument.c_str() + 2;

				if (argument == "extension") {
					if (i <= argc - 2) {
						i++;
						arguments.extensionFilter = argv[i];
						continue;
					}
				} else if (argument == "force_overwrite") {
					arguments.forceOverwrite = true;
					continue;
				} else if (argument == "help") {
					arguments.showHelp = true;
					continue;
				} else if (argument == "ignore_debug_info") {
					arguments.ignoreDebugInfo = true;
					continue;
				} else if (argument == "minimize_diffs") {
					arguments.minimizeDiffs = true;
					continue;
				} else if (argument == "output") {
					if (i <= argc - 2) {
						i++;
						arguments.outputPath = argv[i];
						continue;
					}
				} else if (argument == "silent_assertions") {
					arguments.silentAssertions = true;
					continue;
				} else if (argument == "unrestricted_ascii") {
					arguments.unrestrictedAscii = true;
					continue;
				}
			} else if (argument.size() == 2) {
				switch (argument[1]) {
				case 'e':
					if (i > argc - 2) break;
					i++;
					arguments.extensionFilter = argv[i];
					continue;
				case 'f':
					arguments.forceOverwrite = true;
					continue;
				case '?':
				case 'h':
					arguments.showHelp = true;
					continue;
				case 'i':
					arguments.ignoreDebugInfo = true;
					continue;
				case 'm':
					arguments.minimizeDiffs = true;
					continue;
				case 'o':
					if (i > argc - 2) break;
					i++;
					arguments.outputPath = argv[i];
					continue;
				case 's':
					arguments.silentAssertions = true;
					continue;
				case 'u':
					arguments.unrestrictedAscii = true;
					continue;
				}
			}
		}

		return argv[i];
	}

	return nullptr;
}

int main(int argc, char* argv[]) {
	print(std::string(PROGRAM_NAME) + "\nCompiled on " + __DATE__);

	if (char* invalidArgument = parse_arguments(argc, argv)) {
		print("Invalid argument: " + std::string(invalidArgument) + "\nUse -? to show usage and options.");
		return EXIT_FAILURE;
	}

	if (arguments.showHelp) {
		print(
			"Usage: luajit-decompiler INPUT_PATH [options]\n"
			"\n"
			"Available options:\n"
			"  -h, -?, --help\t\tShow this message\n"
			"  -o, --output OUTPUT_PATH\tOverride default output directory\n"
			"  -e, --extension EXTENSION\tOnly decompile files with the specified extension\n"
			"  -s, --silent_assertions\tOnly print assertion error message\n"
			"\t\t\t\t  and auto skip files that fail to decompile\n"
			"  -f, --force_overwrite\t\tAlways overwrite existing files\n"
			"  -i, --ignore_debug_info\tIgnore bytecode debug info\n"
			"  -m, --minimize_diffs\t\tOptimize output formatting to help minimize diffs\n"
			"  -u, --unrestricted_ascii\tDisable default UTF-8 encoding and string restrictions"
		);
		return EXIT_SUCCESS;
	}

	if (!arguments.inputPath.size()) {
		print("No input path specified!\nUse -? to show usage and options.");
		return EXIT_FAILURE;
	}

	std::filesystem::path outputDir;

	if (!arguments.outputPath.size()) {
		outputDir = std::filesystem::current_path() / "output";
	} else {
		outputDir = std::filesystem::path(arguments.outputPath);

		if (std::filesystem::exists(outputDir)) {
			if (!std::filesystem::is_directory(outputDir)) {
				print("Output path is not a folder: " + arguments.outputPath);
				return EXIT_FAILURE;
			}
		} else {
			std::filesystem::create_directories(outputDir);
		}
	}

	std::filesystem::create_directories(outputDir);

	if (arguments.extensionFilter.size()) {
		if (arguments.extensionFilter.front() != '.') arguments.extensionFilter.insert(arguments.extensionFilter.begin(), '.');
		arguments.extensionFilter = string_to_lowercase(arguments.extensionFilter);
	}

	const std::filesystem::path inputPath(arguments.inputPath);

	if (!std::filesystem::exists(inputPath)) {
		print("Failed to open input path: " + arguments.inputPath);
		return EXIT_FAILURE;
	}

	std::vector<std::filesystem::path> inputs;
	std::filesystem::path inputRoot;

	if (std::filesystem::is_directory(inputPath)) {
		inputRoot = inputPath;

		for (const auto& entry : std::filesystem::recursive_directory_iterator(inputRoot)) {
			if (!entry.is_regular_file()) continue;

			if (arguments.extensionFilter.size()
				&& string_to_lowercase(entry.path().extension().string()) != arguments.extensionFilter) continue;

			inputs.push_back(entry.path());
		}

		if (inputs.empty()) {
			print("No files " + (arguments.extensionFilter.size() ? "with extension " + arguments.extensionFilter + " " : "")
				+ "found in path: " + arguments.inputPath);
			return EXIT_FAILURE;
		}
	} else {
		inputRoot = inputPath.parent_path();
		inputs.push_back(inputPath);
	}

	std::sort(inputs.begin(), inputs.end());

	for (const auto& inputFile : inputs) {
		const std::filesystem::path relativePath = std::filesystem::relative(inputFile, inputRoot);
		const std::filesystem::path outputFile = outputDir
			/ relativePath.parent_path()
			/ output_name_for(relativePath);

		std::filesystem::create_directories(outputFile.parent_path());
		decompile_file(inputFile, outputFile);
	}

	print("--------------------\n"
		+ (filesSkipped ? "Failed to decompile " + std::to_string(filesSkipped) + " file" + (filesSkipped > 1 ? "s" : "") + ".\n" : "")
		+ "Done!");

	return EXIT_SUCCESS;
}

void print(const std::string& message) {
	std::fprintf(stdout, "%s\n", message.c_str());
	std::fflush(stdout);
}

void print_progress_bar(const double& progress, const double& total) {
	static char PROGRESS_BAR[] = "\r[====================]";

	const uint8_t threshold = std::round(20 / total * progress);

	for (uint8_t i = 20; i--;) {
		PROGRESS_BAR[i + 2] = i < threshold ? '=' : ' ';
	}

	std::fprintf(stderr, "%s", PROGRESS_BAR);
	isProgressBarActive = true;
}

void erase_progress_bar() {
	static constexpr char PROGRESS_BAR_ERASER[] = "\r                      \r";

	if (!isProgressBarActive) return;
	std::fprintf(stderr, "%s", PROGRESS_BAR_ERASER);
	isProgressBarActive = false;
}

void assert(const bool& assertion, const std::string& message, const std::string& filePath, const std::string& function, const std::string& source, const uint32_t& line) {
	if (!assertion) throw Error{
		.message = message,
		.filePath = filePath,
		.function = function,
		.source = source,
		.line = std::to_string(line)
	};
}

std::string byte_to_string(const uint8_t& byte) {
	char string[] = "0x00";
	uint8_t digit;

	for (uint8_t i = 2; i--;) {
		digit = (byte >> i * 4) & 0xF;
		string[3 - i] = digit >= 0xA ? 'A' + digit - 0xA : '0' + digit;
	}

	return string;
}
