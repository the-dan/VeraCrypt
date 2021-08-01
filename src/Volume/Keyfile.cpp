/*
 Derived from source code of TrueCrypt 7.1a, which is
 Copyright (c) 2008-2012 TrueCrypt Developers Association and which is governed
 by the TrueCrypt License 3.0.

 Modifications and additions to the original source code (contained in this file)
 and all other portions of this file are Copyright (c) 2013-2017 IDRIX
 and are governed by the Apache License 2.0 the full text of which is
 contained in the file License.txt included in VeraCrypt binary and source
 code distribution packages.
*/

#include "Platform/Serializer.h"
#include "Common/SecurityToken.h"
#include "Crc32.h"
#include "Keyfile.h"
#include "VolumeException.h"

namespace VeraCrypt
{
	void Keyfile::Apply (const BufferPtr &pool, wstring tokenKeyDescriptor, ApplyMode applyMode) const
	{
		if (Path.IsDirectory())
			throw ParameterIncorrect (SRC_POS);

		File file;

		Crc32 crc32;
		size_t poolPos = 0;
		uint64 totalLength = 0;
		uint64 readLength;

		SecureBuffer keyfileBuf (File::GetOptimalReadSize());
		File encryptedKeyfile;


		if (SecurityToken::IsKeyfilePathValid (Path))
		{
			// Apply keyfile generated by a security token
			vector <byte> keyfileData;
			SecurityToken::GetKeyfileData (SecurityTokenKeyfile (wstring (Path)), keyfileData);

			if (keyfileData.size() < MinProcessedLength)
				throw InsufficientData (SRC_POS, Path);

			for (size_t i = 0; i < keyfileData.size(); i++)
			{
				uint32 crc = crc32.Process (keyfileData[i]);

				pool[poolPos++] += (byte) (crc >> 24);
				pool[poolPos++] += (byte) (crc >> 16);
				pool[poolPos++] += (byte) (crc >> 8);
				pool[poolPos++] += (byte) crc;

				if (poolPos >= pool.Size())
					poolPos = 0;

				if (++totalLength >= MaxProcessedLength)
					break;
			}

			burn (&keyfileData.front(), keyfileData.size());
			goto done;
		}

		


		file.Open (Path, File::OpenRead, File::ShareRead);
		trace_msg("Opened keyfile");
		trace_msgw("Using security token key :" << tokenKeyDescriptor);

		

		if (!tokenKeyDescriptor.empty()) {
			// if token is specified, first part of the file is/should be encrypted

			if (applyMode == ApplyMode::CREATE) {
				trace_msg("Using token key to encrypt keyfiles");
			} else {
				trace_msg("Using token key to decrypt keyfiles");
			}
			
			// get token slot and key from descriptor
			SecurityTokenKey key;
			SecurityToken::GetSecurityTokenKey(tokenKeyDescriptor, key, applyMode);

			
			// set proper vector size based on key length
			// and mode (encryption/decryption)
			size_t inputBufferSize = key.maxEncryptBufferSize;
			size_t outputBufferSize = key.maxDecryptBufferSize;
			if (applyMode == ApplyMode::CREATE) {
				inputBufferSize = key.maxDecryptBufferSize;
				outputBufferSize = key.maxEncryptBufferSize;
			}
			uint64 appendBytesCount;

			vector<byte> tokenDataToProcess;
			vector<byte> processedData(outputBufferSize);


			while ((readLength = file.Read (keyfileBuf)) > 0)
			{
				if (tokenDataToProcess.size() < inputBufferSize) {
					appendBytesCount = readLength;
					if (tokenDataToProcess.size() + appendBytesCount > inputBufferSize) {
						appendBytesCount = inputBufferSize - tokenDataToProcess.size();
						tokenDataToProcess.insert(tokenDataToProcess.end(), keyfileBuf.Ptr(), keyfileBuf.Ptr() + appendBytesCount);
						break;
					}
					tokenDataToProcess.insert(tokenDataToProcess.end(), keyfileBuf.Ptr(), keyfileBuf.Ptr() + appendBytesCount);
				}
			}

			string s(tokenDataToProcess.begin(), tokenDataToProcess.end());
			trace_msgw("Keyfile data to be processed by token. Size: " << tokenDataToProcess.size() << L">" << s << L"<");

			switch (applyMode) {
				case ApplyMode::CREATE:
					SecurityToken::GetEncryptedData(key, tokenDataToProcess, processedData);

					// save encrypted data to a new file
					{
						FilesystemPath encryptedKeyfilePath = FilesystemPath(((wstring)Path) + L".vc");

						encryptedKeyfile.Open (encryptedKeyfilePath, File::CreateWrite, File::ShareReadWriteIgnoreLock);
						encryptedKeyfile.Write(ConstBufferPtr(processedData.data(), processedData.size()));

						// When creating we just need to apply original file's data
						processedData = tokenDataToProcess;
					}
					break;
				case ApplyMode::MOUNT:
					SecurityToken::GetDecryptedData(key, tokenDataToProcess, processedData);
					break;
				default:
					throw ParameterIncorrect (SRC_POS);
					break;
			}
			

			for (size_t i = 0; i < processedData.size(); i++)
			{
				uint32 crc = crc32.Process (processedData[i]);

				pool[poolPos++] += (byte) (crc >> 24);
				pool[poolPos++] += (byte) (crc >> 16);
				pool[poolPos++] += (byte) (crc >> 8);
				pool[poolPos++] += (byte) crc;

				if (poolPos >= pool.Size())
					poolPos = 0;

				if (++totalLength >= MaxProcessedLength)
					break;
			}

			// process the rest of the buffer as an ordinary (non-encrypted) data
			// otherwise we'd get non-deterministic behavior, because Read() could produce buffers of different sizes

			if (encryptedKeyfile.IsOpen()) {
				// dump read but unused chunk first
				if (appendBytesCount < readLength) {
					string ss(keyfileBuf.Ptr() + appendBytesCount, keyfileBuf.Ptr() + readLength);
					trace_msgw("Appending the rest of keyfile" << L">" << ss << L"<");
					encryptedKeyfile.Write(ConstBufferPtr(&keyfileBuf[appendBytesCount], readLength-appendBytesCount));
				}
			}
			for (size_t i = appendBytesCount; i < readLength; i++)
			{
				// apply the same chunk to produce key
				uint32 crc = crc32.Process (keyfileBuf[i]);

				pool[poolPos++] += (byte) (crc >> 24);
				pool[poolPos++] += (byte) (crc >> 16);
				pool[poolPos++] += (byte) (crc >> 8);
				pool[poolPos++] += (byte) crc;

				if (poolPos >= pool.Size())
					poolPos = 0;

				if (++totalLength >= MaxProcessedLength)
					break;
			}
		}

		trace_msg("Reading keyfile further");


		while ((readLength = file.Read (keyfileBuf)) > 0)
		{
			if (encryptedKeyfile.IsOpen()) {
				encryptedKeyfile.Write(ConstBufferPtr(keyfileBuf));
			}
			for (size_t i = 0; i < readLength; i++)
			{
				uint32 crc = crc32.Process (keyfileBuf[i]);

				pool[poolPos++] += (byte) (crc >> 24);
				pool[poolPos++] += (byte) (crc >> 16);
				pool[poolPos++] += (byte) (crc >> 8);
				pool[poolPos++] += (byte) crc;

				if (poolPos >= pool.Size())
					poolPos = 0;

				if (++totalLength >= MaxProcessedLength)
					goto done;
			}
		}
done:
		if (encryptedKeyfile.IsOpen()) {
			encryptedKeyfile.Close();
		}
		if (totalLength < MinProcessedLength)
			throw InsufficientData (SRC_POS, Path);
	}

	shared_ptr <VolumePassword> Keyfile::ApplyListToPassword (shared_ptr <KeyfileList> keyfiles, shared_ptr <VolumePassword> password,
		wstring tokenDescriptor, ApplyMode applyMode)
	{
		if (!password)
			password.reset (new VolumePassword);

		if (!keyfiles || keyfiles->size() < 1)
			return password;

		KeyfileList keyfilesExp;
		HiddenFileWasPresentInKeyfilePath = false;

		// Enumerate directories
		foreach (shared_ptr <Keyfile> keyfile, *keyfiles)
		{
			if (FilesystemPath (*keyfile).IsDirectory())
			{
				size_t keyfileCount = 0;
				foreach_ref (const FilePath &path, Directory::GetFilePaths (*keyfile))
				{
#ifdef TC_UNIX
					// Skip hidden files
					if (wstring (path.ToBaseName()).find (L'.') == 0)
					{
						HiddenFileWasPresentInKeyfilePath = true;
						continue;
					}
#endif
					keyfilesExp.push_back (make_shared <Keyfile> (path));
					++keyfileCount;
				}

				if (keyfileCount == 0)
					throw KeyfilePathEmpty (SRC_POS, FilesystemPath (*keyfile));
			}
			else
			{
				keyfilesExp.push_back (keyfile);
			}
		}

		make_shared_auto (VolumePassword, newPassword);

		if (keyfilesExp.size() < 1)
		{
			newPassword->Set (*password);
		}
		else
		{
			SecureBuffer keyfilePool (password->Size() <= VolumePassword::MaxLegacySize? VolumePassword::MaxLegacySize: VolumePassword::MaxSize);

			// Pad password with zeros if shorter than max length
			keyfilePool.Zero();
			keyfilePool.CopyFrom (ConstBufferPtr (password->DataPtr(), password->Size()));

			// Apply all keyfiles
			foreach_ref (const Keyfile &k, keyfilesExp)
			{
				k.Apply (keyfilePool, tokenDescriptor, applyMode);
			}

			newPassword->Set (keyfilePool);
		}

		return newPassword;
	}

	shared_ptr <KeyfileList> Keyfile::DeserializeList (shared_ptr <Stream> stream, const string &name)
	{
		shared_ptr <KeyfileList> keyfiles;
		Serializer sr (stream);

		if (!sr.DeserializeBool (name + "Null"))
		{
			keyfiles.reset (new KeyfileList);
			foreach (const wstring &k, sr.DeserializeWStringList (name))
				keyfiles->push_back (make_shared <Keyfile> (k));
		}
		return keyfiles;
	}

	void Keyfile::SerializeList (shared_ptr <Stream> stream, const string &name, shared_ptr <KeyfileList> keyfiles)
	{
		Serializer sr (stream);
		sr.Serialize (name + "Null", keyfiles == nullptr);
		if (keyfiles)
		{
			list <wstring> sl;

			foreach_ref (const Keyfile &k, *keyfiles)
				sl.push_back (FilesystemPath (k));

			sr.Serialize (name, sl);
		}
	}

	bool Keyfile::HiddenFileWasPresentInKeyfilePath = false;
}
