# Welcome to Bytelocker

Unlock encrypted system volume automatically during boot using TPM2 (Trusted Platform Module 2.0) protection, very fast.

## How to use:

Install an Arch-like distro with a Calamares installer with system disk encryption. Separate boot partition isn't recommended. EFI partition should have about 50MB extra free space.
Update your system and kernel before setting up Bytelocker.

```bash
sudo ./install   
sudo bytelocker setup1
```
Restart your computer, go to UEFI/BIOS settings, and select the boot entry "_Your_distro_ Bytelocker".
Type your disk encryption password for the last time (you should see the Bytelocker text above the prompt)

```bash
sudo bytelocker setup2
```
When you reboot again, no encryption password will be asked. 
A lock screen password is recommended now.

For use with multiple OS-s on the same EFI partition, feel free to modify the #defines in the beginning of the C++ source.

## Updating the kernel or initramfs

Once you are done updating, run sudo bytelocker setup1 again, reboot and run setup2 again.

## Troubleshooting

If your bytelocker EFI doesn't boot, chose the original EFI with boot manager and use password.

If bytelocker fails because TPM memory is full, delete some keys left over from previous, deleted OS-es.
You can list keys by 
```bash
sudo tpm2_getcap handles-persistent
```
and delete keys using 
```bash
sudo tpm2_evictcontrol -C o -c 0x8100????
```

Also delete EFI entries from previous installations:
```bash
efibootmgr -v   
sudo efibootmgr -B b [hexnum]
```

## How it works:

When you install Manjaro/Endevour or similar with whole disk encryption, no separate boot partition, a LUKS key will be placed in /crypto_keyfile.bin, which can also be used to unlock the root disk and swap volumes. This file is in encrypted space, and it's used for remounting root and for swap partition without asking your passphrase more times during boot.
The key in crypto_keyfile.bin  is too large to upload to the TPM2, so Bytelocker, will generate a smaller key and add it to the root LUKS volume, and save it as /crypto_keyfile_bytelocker.bin. This key will have lower iteration count for faster unlocking. Slowing down brute force attacks is necessary for short passphrase keys. For 256 bit random binary keys it will take 2*10^68 years to try all possibilities.
For faster unlocking, bytelocker moves your passphrase to another slot. Slot 0 unlocks the fastest.
After a Calamares installation:
 LUKS slot 0  <-   passphrase chosen during installation
 LUKS slot 1  <-   /crypto_keyfile.bin

After bytelocker setup1:
 LUKS slot 0  <-   /crypto_keyfile_bytelocker.bin
 LUKS slot 1  <-   /crypto_keyfile.bin
 LUKS slot 2  <-   passphrase chosen during installation
 
The TPM2 NVRAM location is allocated and the key is uploaded (though it can't be used yet)
Arch like distros include a copy of /crypto_keyfile.bin in initramfs too. Therefore Bytelocker builds a custom initramfs, without this keyfile, as EFI images are unencrypted. it also adds a hook for retrieving the key from the TPM2 and saving it into the ramdisk named /crypto_keyfile.bin (confusing key filenames, but the other hooks search for this filename). 
The swap volume, if there is one, gets unlocked by the original /crypto_keyfile.bin, because at this stage the root file system is mounted already and linux will see the real crypto_keyfile.bin, and use it to mount the swap partition. Only mkinitcpio systems are supported.
Then it builds an EFI image using the highest version number kernel it finds in /boot, micro code driver and the custom initramfs included. This EFI image is saved at "/boot/efi/EFI/Bytelocker/Linux.efi"
Bytelocker adds an EFI boot entry by invoking efibootmgr.
You can reboot now with the new EFI, and the hook "encrypt" will ask for your password, as the PCRs don't match.

setup2:
Uploads /crypto_keyfile_bytelocker.bin the the TPM2 sealed by the current PCR values. PCR bank 4 value is a hash of the EFI you used to boot. It's set by the UEFI or BIOS before booting.
Attackers can guess the PCR values by hashing your EFI image. But the TPM2 will only release the key, if the PCR values are the same as they were when running setup2.
If the content of Linux.efi or the UEFI settings or firmware changes, you will be asked for your password again (kernel update?). You can run setup2 again to upload the same /crypto_keyfile_bytelocker.bin key with the new PCR values. It will overwrite the previous value in the same NVRAM location. You can find the NVRAm address at /var/lib/bytelocker/nvpersistentaddress.dat

To see the executed comand lines, set "bool verbose = true;" in the source;
Plymouth screens are removed, because they may crash.

Tested with Manjaro and Endeavour installations.
