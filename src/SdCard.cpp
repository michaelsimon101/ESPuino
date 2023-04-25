#include <Arduino.h>
#include "settings.h"
#include "SdCard.h"
#include "Common.h"
#include "Led.h"
#include "Log.h"
#include "MemX.h"
#include "System.h"

#ifdef SD_MMC_1BIT_MODE
	fs::FS gFSystem = (fs::FS)SD_MMC;
#else
	SPIClass spiSD(HSPI);
	fs::FS gFSystem = (fs::FS)SD;
#endif

void SdCard_Init(void) {
	#ifndef SINGLE_SPI_ENABLE
		#ifdef SD_MMC_1BIT_MODE
			pinMode(2, INPUT_PULLUP);
			while (!SD_MMC.begin("/sdcard", true)) {
		#else
			pinMode(SPISD_CS, OUTPUT);
			digitalWrite(SPISD_CS, HIGH);
			spiSD.begin(SPISD_SCK, SPISD_MISO, SPISD_MOSI, SPISD_CS);
			spiSD.setFrequency(1000000);
			while (!SD.begin(SPISD_CS, spiSD)) {
		#endif
	#else
		#ifdef SD_MMC_1BIT_MODE
			pinMode(2, INPUT_PULLUP);
			while (!SD_MMC.begin("/sdcard", true)) {
		#else
			while (!SD.begin(SPISD_CS)) {
		#endif
	#endif
				Log_Println((char *) FPSTR(unableToMountSd), LOGLEVEL_ERROR);
				delay(500);
	#ifdef SHUTDOWN_IF_SD_BOOT_FAILS
				if (millis() >= deepsleepTimeAfterBootFails * 1000) {
					Log_Println((char *) FPSTR(sdBootFailedDeepsleep), LOGLEVEL_ERROR);
					esp_deep_sleep_start();
				}
	#endif
			}
}

void SdCard_Exit(void) {
	// SD card goto idle mode
	#ifdef SD_MMC_1BIT_MODE
		SD_MMC.end();
	#endif
}

sdcard_type_t SdCard_GetType(void) {
	sdcard_type_t cardType;
	#ifdef SD_MMC_1BIT_MODE
		Log_Println((char *) FPSTR(sdMountedMmc1BitMode), LOGLEVEL_NOTICE);
		cardType = SD_MMC.cardType();
	#else
		Log_Println((char *) FPSTR(sdMountedSpiMode), LOGLEVEL_NOTICE);
		cardType = SD.cardType();
	#endif
		return cardType;
}

uint64_t SdCard_GetSize() {
	#ifdef SD_MMC_1BIT_MODE
		return SD_MMC.cardSize();
	#else
		return SD.cardSize();
	#endif
}

uint64_t SdCard_GetFreeSize() {
	#ifdef SD_MMC_1BIT_MODE
		return SD_MMC.cardSize() - SD_MMC.usedBytes();
	#else
		return SD.cardSize() - SD.usedBytes();
	#endif
}

void SdCard_PrintInfo() {
	// show SD card type
	sdcard_type_t cardType = SdCard_GetType();
	Log_Print((char *) F("SD card type: "), LOGLEVEL_DEBUG, true);
	if (cardType == CARD_MMC) {
		Log_Println((char *) F("MMC"), LOGLEVEL_DEBUG);
	} else if (cardType == CARD_SD) {
		Log_Println((char *) F("SDSC"), LOGLEVEL_DEBUG);
	} else if (cardType == CARD_SDHC) {
		Log_Println((char *) F("SDHC"), LOGLEVEL_DEBUG);
	} else {
		Log_Println((char *) F("UNKNOWN"), LOGLEVEL_DEBUG);
	}
	// show SD card size / free space
	uint64_t cardSize = SdCard_GetSize() / (1024 * 1024);
	uint64_t freeSize = SdCard_GetFreeSize() / (1024 * 1024);;
	Log_Printf(LOGLEVEL_NOTICE, sdInfo, cardSize, freeSize);
}


// Check if file-type is correct
bool fileValid(const char *_fileItem) {
	// make file extension to lowercase (compare case insenstive)
	char *lFileItem;
	lFileItem = x_strdup(_fileItem);
	if (lFileItem == NULL) {
		return false;
	}
	lFileItem = strlwr(lFileItem);
	const char ch = '/';
	char *subst;
	subst = strrchr(lFileItem, ch); // Don't use files that start with .
	bool isValid = (!startsWith(subst, (char *) "/.")) && (
			// audio file formats
			endsWith(lFileItem, ".mp3") || 
			endsWith(lFileItem, ".aac") || 
			endsWith(lFileItem, ".m4a") || 
			endsWith(lFileItem, ".wav") || 
			endsWith(lFileItem, ".flac") || 
			endsWith(lFileItem, ".ogg") || 
			endsWith(lFileItem, ".opus") || 
			// playlist file formats
			endsWith(lFileItem, ".m3u") || 
			endsWith(lFileItem, ".m3u8") || 
			endsWith(lFileItem, ".pls") || 
			endsWith(lFileItem, ".asx"));
	free(lFileItem);
	return isValid;
}


// Takes a directory as input and returns a random subdirectory from it
char *SdCard_pickRandomSubdirectory(char *_directory) {
	uint32_t listStartTimestamp = millis();

	// Look if file/folder requested really exists. If not => break.
	File directory = gFSystem.open(_directory);
	if (!directory) {
		Log_Println((char *) FPSTR(dirOrFileDoesNotExist), LOGLEVEL_ERROR);
		return NULL;
	}
	Log_Printf(LOGLEVEL_NOTICE, tryToPickRandomDir, _directory);

	static uint8_t allocCount = 1;
	uint16_t allocSize = psramInit() ? 65535 : 1024;   // There's enough PSRAM. So we don't have to care...
	uint16_t directoryCount = 0;
	char *buffer = _directory;  // input char* is reused as it's content no longer needed
	char *subdirectoryList = (char *) x_calloc(allocSize, sizeof(char));

	if (subdirectoryList == NULL) {
		Log_Println((char *) FPSTR(unableToAllocateMemForLinearPlaylist), LOGLEVEL_ERROR);
		System_IndicateError();
		return NULL;
	}

	// Create linear list of subdirectories with #-delimiters
	while (true) {
		#if defined(HAS_FILEEXPLORER_SPEEDUP) || (ESP_ARDUINO_VERSION_MAJOR == 2 && ESP_ARDUINO_VERSION_MINOR == 0 && ESP_ARDUINO_VERSION_PATCH >= 8)
			bool isDir = false;
			String MyfileName = directory.getNextFileName(&isDir);
			if (MyfileName == "") {
				break;
			}
			if (isDir) {
				continue;
			} else {
			strncpy(buffer, (char *) MyfileName.c_str(), 255);
		#else
			File fileItem = directory.openNextFile();
			if (!fileItem) {
				break;
			}
			if (!fileItem.isDirectory()) {
				continue;
			} else {
			#if ESP_ARDUINO_VERSION_MAJOR >= 2
				strncpy(buffer, (char *) fileItem.path(), 255);
			#else
				strncpy(buffer, (char *) fileItem.name(), 255);
			#endif
		#endif

			// Log_Printf(LOGLEVEL_INFO, nameOfFileFound, buffer);
			if ((strlen(subdirectoryList) + strlen(buffer) + 2) >= allocCount * allocSize) {
				char *tmp = (char *) realloc(subdirectoryList, ++allocCount * allocSize);
				Log_Println((char *) FPSTR(reallocCalled), LOGLEVEL_DEBUG);
				if (tmp == NULL) {
					Log_Println((char *) FPSTR(unableToAllocateMemForLinearPlaylist), LOGLEVEL_ERROR);
					System_IndicateError();
					free(subdirectoryList);
					return NULL;
				}
				subdirectoryList = tmp;
			}
			strcat(subdirectoryList, stringDelimiter);
			strcat(subdirectoryList, buffer);
			directoryCount++;
		}
	}
	strcat(subdirectoryList, stringDelimiter);

	if (!directoryCount) {
		free(subdirectoryList);
		return NULL;
	}

	uint16_t randomNumber = random(directoryCount) + 1;     // Create random-number with max = subdirectory-count
	uint16_t delimiterFoundCount = 0;
	uint32_t a=0;
	uint8_t b=0;

	// Walk through subdirectory-array and extract randomized subdirectory
	while (subdirectoryList[a] != '\0') {
		if (subdirectoryList[a] == '#') {
			delimiterFoundCount++;
		} else {
			if (delimiterFoundCount == randomNumber) {  // Pick subdirectory of linear char* according to random number
				buffer[b++] = subdirectoryList[a];
			}
		}
		if (delimiterFoundCount > randomNumber || (b == 254)) {  // It's over when next delimiter is found or buffer is full
			buffer[b] = '\0';
			free(subdirectoryList);
			Log_Printf(LOGLEVEL_NOTICE, pickedRandomDir, _directory);
			return buffer;  // Full path of random subdirectory
		}
		a++;
	}

	free(subdirectoryList);
	Log_Printf(LOGLEVEL_DEBUG, "pick random directory from SD-card finished: %lu ms", (millis() - listStartTimestamp));
	return NULL;
}


/* Puts SD-file(s) or directory into a playlist
	First element of array always contains the number of payload-items. */
char **SdCard_ReturnPlaylist(const char *fileName, const uint32_t _playMode) {
	static char **files;
	char *serializedPlaylist = NULL;
	char fileNameBuf[255];
	char cacheFileNameBuf[275];
	bool readFromCacheFile = false;
	bool enablePlaylistCaching = false;
	bool enablePlaylistFromM3u = false;
	uint32_t listStartTimestamp = millis();

	if (files != NULL) { // If **ptr already exists, de-allocate its memory
		Log_Println((char *) FPSTR(releaseMemoryOfOldPlaylist), LOGLEVEL_DEBUG);
		freeMultiCharArray(files, strtoul(files[0], NULL, 10) + 1);
		Log_Printf(LOGLEVEL_DEBUG, freeMemoryAfterFree, ESP.getFreeHeap());
	}

	// Look if file/folder requested really exists. If not => break.
	File fileOrDirectory = gFSystem.open(fileName);
	if (!fileOrDirectory) {
		Log_Println((char *) FPSTR(dirOrFileDoesNotExist), LOGLEVEL_ERROR);
		return nullptr;
	}

	// Create linear playlist of caching-file
	#ifdef CACHED_PLAYLIST_ENABLE
		strncpy(cacheFileNameBuf, fileName, sizeof(cacheFileNameBuf));
		strcat(cacheFileNameBuf, "/");
		strcat(cacheFileNameBuf, (const char*) FPSTR(playlistCacheFile));       // Build absolute path of cacheFile

		// Decide if to use cacheFile. It needs to exist first...
		if (gFSystem.exists(cacheFileNameBuf)) {     // Check if cacheFile (already) exists
			readFromCacheFile = true;
		}

		// ...and playmode has to be != random/single (as random along with caching doesn't make sense at all)
		if (_playMode == SINGLE_TRACK ||
			_playMode == SINGLE_TRACK_LOOP) {
				readFromCacheFile = false;
		} else {
			enablePlaylistCaching = true;
		}

		// Read linear playlist (csv with #-delimiter) from cachefile (faster!)
		if (readFromCacheFile) {
			File cacheFile = gFSystem.open(cacheFileNameBuf);
			if (cacheFile) {
				uint32_t cacheFileSize = cacheFile.size();

				if (!(cacheFileSize >= 1)) {        // Make sure it's greater than 0 bytes
					Log_Println((char *) FPSTR(playlistCacheFoundBut0), LOGLEVEL_ERROR);
					readFromCacheFile = false;
				} else {
					Log_Println((char *) FPSTR(playlistGenModeCached), LOGLEVEL_NOTICE);
					serializedPlaylist = (char *) x_calloc(cacheFileSize+10, sizeof(char));
					if (serializedPlaylist == NULL) {
						Log_Println((char *) FPSTR(unableToAllocateMemForLinearPlaylist), LOGLEVEL_ERROR);
						System_IndicateError();
						return nullptr;
					}

					char buf;
					uint32_t fPos = 0;
					while (cacheFile.available() > 0) {
						buf = cacheFile.read();
						serializedPlaylist[fPos++] = buf;
					}
				}
				cacheFile.close();
			}
		}
	#endif

	Log_Printf(LOGLEVEL_DEBUG, freeMemory, ESP.getFreeHeap());

	// Parse m3u-playlist and create linear-playlist out of it
	if (_playMode == LOCAL_M3U) {
		if (fileOrDirectory && !fileOrDirectory.isDirectory() && fileOrDirectory.size() > 0) {
			enablePlaylistFromM3u = true;
			uint16_t allocCount = 1;
			uint16_t allocSize = psramInit() ? 65535 : 1024;   // There's enough PSRAM. So we don't have to care...

			serializedPlaylist = (char *) x_calloc(allocSize, sizeof(char));
			if (serializedPlaylist == NULL) {
				Log_Println((char *) FPSTR(unableToAllocateMemForLinearPlaylist), LOGLEVEL_ERROR);
				System_IndicateError();
				return nullptr;
			}
			char buf;
			char lastBuf = '#';
			uint32_t fPos = 1;

			serializedPlaylist[0] = '#';
			while (fileOrDirectory.available() > 0) {
				buf = fileOrDirectory.read();
				if (buf == '#') {
					// skip M3U comment lines starting with #
					fileOrDirectory.readStringUntil('\n');
					continue;
				}				
				if (fPos+1 >= allocCount * allocSize) {
					char *tmp = (char *) realloc(serializedPlaylist, ++allocCount * allocSize);
					Log_Println((char *) FPSTR(reallocCalled), LOGLEVEL_DEBUG);
					if (tmp == NULL) {
						Log_Println((char *) FPSTR(unableToAllocateMemForLinearPlaylist), LOGLEVEL_ERROR);
						System_IndicateError();
						free(serializedPlaylist);
						return nullptr;
					}
					serializedPlaylist = tmp;
				}

				if (buf != '\n' && buf != '\r') {
					serializedPlaylist[fPos++] = buf;
					lastBuf = buf;
				} else {
					if (lastBuf != '#') {   // Strip empty lines from m3u
						serializedPlaylist[fPos++] = '#';
						lastBuf = '#';
					}
				}
			}
			if (serializedPlaylist[fPos-1] == '#') {    // Remove trailing delimiter if set
				serializedPlaylist[fPos-1] = '\0';
			}
		} else {
			return nullptr;
		}
	}

	// Don't read from cachefile or m3u-file. Means: read filenames from SD and make playlist of it
	if (!readFromCacheFile && !enablePlaylistFromM3u) {
		Log_Println((char *) FPSTR(playlistGenModeUncached), LOGLEVEL_NOTICE);
		// File-mode
		if (!fileOrDirectory.isDirectory()) {
			files = (char **) x_malloc(sizeof(char *) * 2);
			if (files == nullptr) {
				Log_Println((char *) FPSTR(unableToAllocateMemForPlaylist), LOGLEVEL_ERROR);
				System_IndicateError();
				return nullptr;
			}
			Log_Println((char *) FPSTR(fileModeDetected), LOGLEVEL_INFO);
			#if ESP_ARDUINO_VERSION_MAJOR >= 2
				strncpy(fileNameBuf, (char *) fileOrDirectory.path(), sizeof(fileNameBuf) / sizeof(fileNameBuf[0]));
			#else
				strncpy(fileNameBuf, (char *) fileOrDirectory.name(), sizeof(fileNameBuf) / sizeof(fileNameBuf[0]));
			#endif
			if (fileValid(fileNameBuf)) {
				files[1] = x_strdup(fileNameBuf);
			}
			files[0] = x_strdup("1"); // Number of files is always 1 in file-mode

			return &(files[1]);
		}

		// Directory-mode (linear-playlist)
		uint16_t allocCount = 1;
		uint16_t allocSize = 4096;
		if (psramInit()) {
			allocSize = 65535; // There's enough PSRAM. So we don't have to care...
		}

		serializedPlaylist = (char *) x_calloc(allocSize, sizeof(char));
		File cacheFile;
		if (enablePlaylistCaching) {
			cacheFile = gFSystem.open(cacheFileNameBuf, FILE_WRITE);
		}

		while (true) {
			#if defined(HAS_FILEEXPLORER_SPEEDUP) || (ESP_ARDUINO_VERSION_MAJOR == 2 && ESP_ARDUINO_VERSION_MINOR == 0 && ESP_ARDUINO_VERSION_PATCH >= 8)
				bool isDir = false;
				String MyfileName = fileOrDirectory.getNextFileName(&isDir);
				if (MyfileName == "") {
					break;
				}
				if (isDir) {
					continue;
				} else {
				strncpy(fileNameBuf, (char *) MyfileName.c_str(), sizeof(fileNameBuf) / sizeof(fileNameBuf[0]));
			#else
				File fileItem = fileOrDirectory.openNextFile();
				if (!fileItem) {
					break;
				}
				if (fileItem.isDirectory()) {
					continue;
				} else {
				#if ESP_ARDUINO_VERSION_MAJOR >= 2
					strncpy(fileNameBuf, (char *) fileItem.path(), sizeof(fileNameBuf) / sizeof(fileNameBuf[0]));
				#else
					strncpy(fileNameBuf, (char *) fileItem.name(), sizeof(fileNameBuf) / sizeof(fileNameBuf[0]));
				#endif
			#endif
				// Don't support filenames that start with "." and only allow .mp3 and other supported audio file formats
				if (fileValid(fileNameBuf)) {
					// Log_Printf(LOGLEVEL_INFO, "%s: %s", (char *) FPSTR(nameOfFileFound), fileNameBuf);
					if ((strlen(serializedPlaylist) + strlen(fileNameBuf) + 2) >= allocCount * allocSize) {
						char *tmp = (char *) realloc(serializedPlaylist, ++allocCount * allocSize);
						Log_Println((char *) FPSTR(reallocCalled), LOGLEVEL_DEBUG);
						if (tmp == nullptr) {
							Log_Println((char *) FPSTR(unableToAllocateMemForLinearPlaylist), LOGLEVEL_ERROR);
							System_IndicateError();
							free(serializedPlaylist);
							return nullptr;
						}
						serializedPlaylist = tmp;
					}
					strcat(serializedPlaylist, stringDelimiter);
					strcat(serializedPlaylist, fileNameBuf);
					if (cacheFile && enablePlaylistCaching) {
						cacheFile.print(stringDelimiter);
						cacheFile.print(fileNameBuf);       // Write linear playlist to cacheFile
					}
				}
			}
		}

		if (cacheFile && enablePlaylistCaching) {
			cacheFile.close();
		}
	}

	// Get number of elements out of serialized playlist
	uint32_t cnt = 0;
	for (uint32_t k = 0; k < (strlen(serializedPlaylist)); k++) {
		if (serializedPlaylist[k] == '#') {
			cnt++;
		}
	}

	// Alloc only necessary number of playlist-pointers
	files = (char **) x_malloc(sizeof(char *) * (cnt + 1));
	if (files == nullptr) {
		Log_Println((char *) FPSTR(unableToAllocateMemForPlaylist), LOGLEVEL_ERROR);
		System_IndicateError();
		free(serializedPlaylist);
		return nullptr;
	}

	// Extract elements out of serialized playlist and copy to playlist
	char *token;
	token = strtok(serializedPlaylist, stringDelimiter);
	uint32_t pos = 1;
	while (token != NULL) {
		files[pos++] = x_strdup(token);
		token = strtok(NULL, stringDelimiter);
	}

	free(serializedPlaylist);

	files[0] = (char *) x_malloc(sizeof(char) * 5);

	if (files[0] == nullptr) {
		Log_Println((char *) FPSTR(unableToAllocateMemForPlaylist), LOGLEVEL_ERROR);
		System_IndicateError();
		freeMultiCharArray(files, cnt + 1);
		return nullptr;
	}
	snprintf(files[0], 5,  "%u", cnt);
	Log_Printf(LOGLEVEL_NOTICE, numberOfValidFiles, cnt);
	Log_Printf(LOGLEVEL_DEBUG, "build playlist from SD-card finished: %lu ms", (millis() - listStartTimestamp));

	return &(files[1]); // return ptr+1 (starting at 1st payload-item); ptr+0 contains number of items
}
