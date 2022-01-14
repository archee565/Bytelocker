# Welcome to Bytelocker Beta!

Unlock encrypted system volume automatically during boot using TPM2 (Trusted Platform Module 2.0) protection, very fast.

## How to use:

Install an Ubuntu- or Arch-like distro with system disk encryption. Separate boot partition isn't recommended.  EFI partition should have 140MB free space for Ubuntu.
Update your system and kernel before setting up Bytelocker.

```bash
sudo ./install   
```
When you reboot again, no encryption password will be asked. 

For having multiple systems on the same EFI partition, you'll need to modify the source.

## Known bugs:

- The EFI partition is assumed to be mounted at /boot/efi, which isn't the case on every Arch linux.
- On Ubuntu/Debian based systems distro upgrade will fail, beacuse tpm2_tools are not accessible in the middle of the upgrade process. Remove the bytelocker hooks before system upgrade, and run "bytelocker install" again after upgrade.
- on Pop OS default install the efi partition size might be too small as the OS grows.

## Troubleshooting

If Bytelocker fails because TPM memory is full, delete some keys left over from previous, deleted OS-es.
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

Some distros create a /crypto_keyfile.bin with an unlock key. By default, this is included in the InitRAMFS, but Bytelocker removes keys form InitRAMFS. As a result you'll be asked password multiple time if you boot with the official EFI image.
The key in crypto_keyfile.bin is too large to upload to the TPM2, so Bytelocker, will generate a smaller key and add it to the root LUKS volume, and save it as /crypto_keyfile_bytelocker.bin. This key will have lower iteration count for faster unlocking. Slowing down brute force attacks is necessary for short passphrase keys. For 256 bit random binary keys it will take 2*10^68 years to try all possibilities.
For faster unlocking, bytelocker moves your passphrase to another slot. Slot 0 unlocks the fastest.

After a Calamares installation:
```
 LUKS slot 0  <-   passphrase chosen during installation
 LUKS slot 1  <-   /crypto_keyfile.bin
```

After bytelocker setup1:
```
 LUKS slot 0  <-   /crypto_keyfile_bytelocker.bin
 LUKS slot 1  <-   /crypto_keyfile.bin
 LUKS slot 2  <-   passphrase chosen during installation
 ```
 
On Ubutnu, it modifies crypttab to call a keyscript instead of using a keyfile. The script will download the key from the TPM2 from the chosen persistent address. 
On Arch an initcpio hook will unseal the key into the ramFS.
The TPM2 will unseal the key only if the PCRs match.
Generates an EFI image, which includes kernel and initRAMFS onto the unencrypted EFI partition.
Adds an EFI boot entry by invoking efibootmgr.
Calculates the future value of PCR4 based on the EFI image.
An update.d hook or a pacman.d hook is added. When the kernel or initRAMFS updates, the custom EFI image will be automatically generated again, and the keyfile will be reuploaded to the TPM using the new PCR values.

***Tested distros:*** Kubuntu, Pop OS, Endaevour OS, Manajaro

## Remarks:
When googling for automatic decryption with TPM setup, many pages contain wrong information. 
Most common flaws:
 - Using only PCR values as key. An attacker can hash your EFI and find the PCR values. Though, it protects better, if you have BIOS password and secure boot setup.
 - Not sealing keys in NVRAM: Using nvread and nvwrite is wrong. The TPM will give out your encryption key even if the attacker boots from USB. 
 - Keeping decryption key files in unencrypted initramfs in EFI or elsewhere. Search for keys within unencrypted initramfs in /cryptroot/keys  and  /.
