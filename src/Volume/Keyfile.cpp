/*
 Derived from source code of TrueCrypt 7.1a, which is
 Copyright (c) 2008-2012 TrueCrypt Developers Association and which is governed
 by the TrueCrypt License 3.0.

 Modifications and additions to the original source code (contained in this file) 
 and all other portions of this file are Copyright (c) 2013-2015 IDRIX
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
	void Keyfile::Apply (const BufferPtr &pool, wstring tokenKeyDescriptor) const
	{
		if (Path.IsDirectory())
			throw ParameterIncorrect (SRC_POS);

		File file;

		Crc32 crc32;
		size_t poolPos = 0;
		uint64 totalLength = 0;
		uint64 readLength;

		SecureBuffer keyfileBuf (File::GetOptimalReadSize());


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

			Memory::Erase (&keyfileData.front(), keyfileData.size());
			goto done;
		}

		


		file.Open (Path, File::OpenRead, File::ShareRead);

		if (tokenKeyDescriptor.size() != 0) {
			// if token is specified, treat first part of the file as encrypted
			// TODO: get token slot and key from descriptor
			vector <byte> tokenDataToDecrypt;
			
			// TODO: set proper vector size based on key length
			
			SecurityTokenKey key;
			SecurityToken::GetSecurityTokenKey(tokenKeyDescriptor, key);
			size_t maxDecryptBufferSize = key.maxDecryptBufferSize;
			uint64 appendBytesCount;


			while ((readLength = file.Read (keyfileBuf)) > 0)
			{
				if (tokenDataToDecrypt.size() < maxDecryptBufferSize) {
					appendBytesCount = readLength;
					if (tokenDataToDecrypt.size() + appendBytesCount > maxDecryptBufferSize) {
						appendBytesCount = maxDecryptBufferSize - tokenDataToDecrypt.size();
						break;
					}
					//TODO: how many bytes will be appended? appendBytesCount or appendBytesCount - 1
					tokenDataToDecrypt.insert(tokenDataToDecrypt.end(), keyfileBuf.Ptr(), keyfileBuf.Ptr() + appendBytesCount);
				}
			}

			vector<byte> decryptedData;
			decryptedData.reserve(maxDecryptBufferSize);

			SecurityToken::GetDecryptedData(key, tokenDataToDecrypt, decryptedData);

			for (size_t i = 0; i < decryptedData.size(); i++)
			{
				uint32 crc = crc32.Process (decryptedData[i]);

				pool[poolPos++] += (byte) (crc >> 24);
				pool[poolPos++] += (byte) (crc >> 16);
				pool[poolPos++] += (byte) (crc >> 8);
				pool[poolPos++] += (byte) crc;

				if (poolPos >= pool.Size())
					poolPos = 0;

				if (++totalLength >= MaxProcessedLength)
					break;
			}

			// process the rest of the buffer as ordinary(non-encrypted) data
			// otherwise we get non-deterministic behavior, because Read() could produce buffers
			// of different sizes

			// TODO: if vector::insert() doesn't push last byte, we don't need to start from appendBytesCount + 1
			for (size_t i = appendBytesCount + 1; i < readLength; i++)
			{
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


		while ((readLength = file.Read (keyfileBuf)) > 0)
		{
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
		if (totalLength < MinProcessedLength)
			throw InsufficientData (SRC_POS, Path);
	}

	shared_ptr <VolumePassword> Keyfile::ApplyListToPassword (shared_ptr <KeyfileList> keyfiles, shared_ptr <VolumePassword> password,
		wstring tokenDescriptor)
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
			SecureBuffer keyfilePool (VolumePassword::MaxSize);

			// Pad password with zeros if shorter than max length
			keyfilePool.Zero();
			keyfilePool.CopyFrom (ConstBufferPtr (password->DataPtr(), password->Size()));

			// Apply all keyfiles
			foreach_ref (const Keyfile &k, keyfilesExp)
			{
				k.Apply (keyfilePool, tokenDescriptor);
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
