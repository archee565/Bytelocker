/*
 bytelocker  
 Sets up an extra EFI boot image to automatically unlock encrypted system volumes
  on startup using Trusted Platform Module 2.0.  For ArchLinux like distros. 

GNU General Public License v3.0

 by Peter Soltesz (archee)  2021     archee565 gmail
 */
 
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <filesystem>
#include <array>
#include <stdexcept>
#include <memory>
#include <map>
#include <cstdlib>
#include <sys/stat.h>

using namespace std;

#define EFI_FILE "/boot/efi/EFI/Bytelocker/Linux.efi"
#define EFI_FILE_EFISTYLE "\\EFI\\BYTELOCKER\\LINUX.EFI"
#define DATADIR "/var/lib/bytelocker"
#define PCR "0,2,4,7"   // PCR 4 is a hash of the EFI file, it's determined by the UEFI Fimrware.
#define LUKSKEY "/crypto_keyfile_bytelocker.bin"
bool verbose = true;
bool optimizeLUKS = true;
bool unplymouth = true;



string shaalg;
string logfile;
string bootOptionName;

void printUsage()
{
    cout << "Usage:\n\nsudo bytelocker setup1\n       Generates an EFI boot binary including the kernel and a custom initramfs.\n       generates new LUKS key and adds it to the system LUKS volume\n       Allocates a slot in the TPM2's NVRAM\n\n";

    cout << "sudo bytelocker setup2\n       Uploads the LUKS key to the TPM2 sealed with the actuals PCRs (run after rebooting)\n\n";
}

bool fileExists(string name)
{
    fstream f;
    f.open(name,fstream::in);
    if (f.fail()) return false;
    f.close();
    return true;
}

 string toDelete;

void error(string errstr)
{
    if (toDelete.length()) filesystem::remove(toDelete);
    cout << logfile;
    cout << "\nbytelocker error: " << errstr << "\n\n";
    exit(-1);
}

void execNoPipe(string cmd)
{
    if (verbose) cout << "*" << cmd << "\n"; // verbose
    else logfile += cmd + "\n";
    system(cmd.c_str());
}

string exec(string cmd) {
    array<char, 1280> buffer;
    string result;
    if (verbose) cout << "*" << cmd << "\n"; // verbose
    else logfile += cmd + "\n";

    unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        throw runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

bool writeToFile(string file,string data)
{
    fstream f;
    f.open(file,fstream::out);
    if (f.fail()) error("can't create files");
    if (data.size()) f.write(&data[0],data.size());
    f.close();
//    chmod(file.c_str(),__S_IREAD|__S_IWRITE); // users should not read the keyfile
    filesystem::permissions(file,filesystem::perms::owner_read|filesystem::perms::owner_write,filesystem::perm_options::replace);
    return true;
}

string loadFile(string file)
{
    string res;
    fstream f;
    f.open(file,fstream::in);
    if (f.fail()) return "";
    f.seekg(0,ios::end);
    streampos length = f.tellg();
    f.seekg(0,ios::beg);
    res.resize(length);
    if (length) f.read(&res[0],length);
    f.close();
    return res;
}

void checkDeps()
{
    if (getuid()!=0)
    {
        error("Must be run as root.");
    }
    if (!fileExists("/usr/bin/mkinitcpio")) error("mkinitcpio is required and only inticpio systems are supported.");
    if (!fileExists("/usr/bin/tpm2_create")) error("tpm2_tools must be installed.");
    if (!fileExists("/usr/bin/objcopy")) error("binutils must be installed.");
    if (!fileExists("/usr/bin/cryptsetup")) error("cryptsetup must be installed.");
    if (!fileExists("/usr/bin/mkinitcpio")) error("mkinitcpio must be installed.");
}

string parseFindHandle(string s)
{
    string s2=s;
    string start = string("persistent-handle: 0x");
    s2.resize(start.length());
    if (s2.compare(start)!=0 || s.find("persisted")==-1)
    {
        error("tpm2_evictcontrol did not return a persistent handle\n");
    };
    string result;
    for(int i=0;i<8+2;i++)
    {
        result.push_back(s[i+start.length()-2]);
    }
    return result;
}

string uploadToTPM(string filename,string nvaddress)
{
    if (nvaddress.length()) // reusing the same NV address location
    { // clear
        string result = exec("tpm2_evictcontrol -C o -c "+nvaddress);
        if (result.find(" evicted")==-1) 
        {
            cout << "Warning! TPM2 failed to delete previous object at" << nvaddress << "\n";
            cout << result << "\n";
            nvaddress = "";
        }
    }

    string result = exec("tpm2_createprimary -c " DATADIR "/primary.ctx -Q");
//    if (result.size()) error("tpm2_createprimary failed");
    result = exec(string("tpm2_createpolicy -Q --policy-pcr --pcr-list ") + shaalg + PCR " --policy " DATADIR "/pcr.policy");
//    if (result.size()) error("tpm2_createpolicy failed");
    result = exec(string("tpm2_create -C " DATADIR "/primary.ctx -L " DATADIR "/pcr.policy -u " DATADIR "/seal.pub -r " DATADIR "/seal.priv -Q --sealing-input ")+filename);
//    if (result.size()) error("tpm2_create failed");
    result = exec(string("tpm2_load -C " DATADIR "/primary.ctx -u " DATADIR "/seal.pub -r " DATADIR "/seal.priv -c " DATADIR "/seal.ctx -Q"));

    result = exec(string("tpm2_evictcontrol --object-context " DATADIR "/seal.ctx --hierarchy o --output " DATADIR "/evict.dat ")+nvaddress);
//    string result;

    return parseFindHandle(result);
}

string testunseal(string nvaddress)
{
    exec(string("tpm2_unseal -c "+nvaddress+" -p pcr:") + shaalg +  PCR " -o /tmp/sealtest.bin");
    string res = loadFile("/tmp/sealtest.bin");
    filesystem::remove("/tmp/sealtest.bin");
    return res;
}

// NVRAM address needs to be allocated during setup1, because address must be hardcoded into the initramfs. Updating initramfs breaks the PCR hashes.
string findNVPersistentAddress(bool setup1)
{
    string res;
    res = loadFile(DATADIR "/nvpersistentaddress.dat");
    if (res!="") return res;

    if (setup1==false) error("Can't find NVRAM persistent address record. Have you run setup1 before setup2?");

    res = uploadToTPM(LUKSKEY,""); // test TPM
    string enckey2 = loadFile(LUKSKEY);
    string enckey = testunseal(res);
    if (enckey2.compare(enckey)!=0) error("TPM2 testing failed");

    writeToFile(DATADIR "/nvpersistentaddress.dat",res);

    cout << "NVRAM address allocated: " << res << "\n";
    return res;
}

void stringReplace(string &s,string a,string b)
{
    int j;
    while(true)
    {
        j = s.find(a);
        if (j!=-1)
        {
            s.replace(j,a.length(),b);
        }
        else
        break;
    }
}

void genInitRamFS(string imagefile)
{
    // Manjaro installer builds the /crypto_keyfile.bin into the official initramfs
    // so we have to generate a custom initramfs

    string conffile = DATADIR "/mkinitcpio-bytelocker.conf";

    fstream forig;
    forig.open("/etc/mkinitcpio.conf",fstream::in);
    if (forig.fail()) error("Can't open /etc/mkinitcpio.conf\nOnly mkinitcpio systems are supported.");

    fstream fnew;
    fnew.open(conffile,fstream::out);

    forig.clear();

    bool modded=false;
    while(forig.eof()==false)
    {
        string line;
        getline(forig,line);

        string line2;
        line2.clear();
        int i=0;
        while(line[i]==' ' || line[i]=='\t') i++; // remove spaces
        for(;i<line.size();i++) // removing comments
        {
            if (line[i]=='#') break;
            line2.push_back(line[i]);
        }
        line = line2;


        if (line.find("/crypto_keyfile.bin")!=-1)  
        {
            // we do not need this key, and it would be insecure too.
            line = "#" + line + " #removed by bytelocker";
        }

        if (line.rfind("HOOKS=",0)==0)
        {
            if (line.find("bytelocker")!=-1) error("Default etc/mkinitcpio.conf must not refer to bytelocker");

            if (unplymouth)
            {
                stringReplace(line,"plymouth-encrypt","encrypt"); // plymouth seems to crash it
                stringReplace(line,"plymouth ","");
            }

            int j = line.find(" encrypt ");
            if (j==-1) j = line.find(" plymouth-encrypt ");
            
            if (j!=-1)
            {
                line.insert(j," bytelocker");
                modded = true;
                line = line + " # edited by bytelocker";
            }
        }

        if (line.size()) line.push_back('\n');        
        fnew.write(&line[0],line.size());
    }

    fnew.close();

    if (modded==false) error("Could not edit HOOKS in custom mkinitcpio.conf");

    filesystem::remove(imagefile);

    string res;
    res = exec(string("mkinitcpio -g ")+imagefile+" -c "+conffile);
//    res = exec(string("mkinitcpio -g ")+imagefile+" -c /etc/mkinitcpio.conf");

    if (!fileExists(imagefile)) error(string("mkinitcpio failed:\n")+res);

}

string findKernel()
{
    string result;
    
    // newest kernel is last in alphabetic order
    string bestkernel;
    double bestscore=-1.;
    map<string,int> sorted;

    for(const auto filename : filesystem::directory_iterator("/boot"))
    {
        string ker = filename.path().string();
        if (ker.find("/vmlinuz")!=-1) 
        {
            double score = 0;
            for(int i=0;i<ker.length();i++)
            {
                if (ker[i]>='0' && ker[i]<='9') score=score+10.+double(ker[i]-'0');
                else score*=10.;
            }
            if (score>bestscore)
            {
                bestscore = score;
                bestkernel =  ker;
            }
            sorted[filename.path().string()] = 1;
        }
    }

    cout<<"Using kernel:\n";
    for(auto it = sorted.begin(); it!=sorted.end(); it++)
    {
        if (bestkernel.compare(it->first)==0) 
        {
            cout << " [X] ";
         } else cout << " [ ] ";

        cout << it->first << "\n";
    }

    if (bestkernel.empty()) error("Can't find kernel");
    return bestkernel;
}


void installHooks(string nvaddress)
{
    string hook = string(
        "#!/usr/bin/bash\n"
        "run_hook() {\n"
        "    modprobe -a -q tpm_crb >/dev/null 2>&1\n"
        "    tpm2_unseal -c "+nvaddress+" -p pcr:") + shaalg+PCR+" -o /crypto_keyfile.bin >/dev/null 2>&1\n"
"if [ ! -f \"/crypto_keyfile.bin\" ]\n"
" then\n"
" echo Bytelocker could not decrypt the drive automatically\n"
" echo    - booting for the first time, before uploading the keys\n"
" echo    - updated kernel and/or initramfs\n"
" echo    - something changed in the system\n"
" echo\n"
" echo\n"
" fi\n"
"}\n";

    string installer =
        "#!/bin/bash\n"
        "build() {\n"
        "    local mod\n"
        "    add_module \"tpm_crb\"\n"
        "    add_binary \"tpm2_unseal\"\n"
        "    add_binary \"/usr/lib/libtss2-tcti-device.so\"\n"
        "    add_runscript\n"
        "}\n"
        "help() {\n"
        "\tcat <<HELPEOF\n"
        "retrives the system disk encryption key from the TPM2, which will unseal it if the PCRs match\n"
        "HELPEOF\n"
        "}\n";

    writeToFile("/usr/lib/initcpio/hooks/bytelocker",hook);
    writeToFile("/usr/lib/initcpio/install/bytelocker",installer);

}


void genEFI(string kernel,string imagefile)
{

    filesystem::create_directory("/boot/efi/EFI/Bytelocker");

    fstream f;
    f.open("/proc/cmdline");
    string cmdline;
    while(!f.eof())
    {
        char c = f.get();
        if (c<32) break;
        cmdline.push_back(c );
    }
    f.close();
//    cout << cmdline + "\n";
    writeToFile(DATADIR "/kernel-command-line.txt",cmdline);

    string ucode; // combining microcode firmware into the initramfs
    if (fileExists("/boot/amd-ucode.img")) ucode = "/boot/amd-ucode.img";
    if (fileExists("/boot/intel-ucode.img")) ucode = "/boot/intel-ucode.img";
    if (ucode.length())
    {
        string combined = loadFile(ucode);
        combined+=loadFile(imagefile);
        string origImageFile = imagefile;
        imagefile = DATADIR "/initramfs-ucode-bytelocker.img";
        writeToFile(imagefile,combined);
//        filesystem::remove(origImageFile); // cleanup
    }

    execNoPipe(string("objcopy --add-section .osrel=\"/usr/lib/os-release\" --change-section-vma .osrel=0x30000 "
    "--add-section .cmdline=\"" DATADIR "/kernel-command-line.txt\" --change-section-vma .cmdline=0x38000 "
    "--add-section .linux=\"") + kernel + string("\" --change-section-vma .linux=0x40000 "
    "--add-section .initrd=\"") + imagefile+  string("\" --change-section-vma .initrd=0x4000000 ") +  // leaving 50MB space for initramfs
    string("\"/usr/lib/systemd/boot/efi/linuxx64.efi.stub\" ") + 
     "\"" EFI_FILE "\""
    );

//    filesystem::remove(imagefile); // cleanup

    if (!fileExists(EFI_FILE)) error("Failed to save EFI file to EFI partition");
}

string getDistroName()
{
    string data = loadFile("/etc/os-release");
    int i=data.find("NAME=");
    if (i<0) return "Linux";
    i+=5;
    while(i<data.length() && data[i]!='"' && data[i]!='\'') i++;
    i++;
    string res;
    while(i<data.length() && data[i]!='"' && data[i]!='\'')
    {
        if (data[i]<32) break;
        res.push_back(data[i]);
        i++;
    }

    stringReplace(res," Linux","");
    return res;
}


void setBootManager()
{
    if (fileExists(DATADIR "/boot_menu_installed_cmd")) return; // boot manager already has been updated

    string efidev = exec("mount | grep \" on /boot/efi \"");
    int i=0;
    for(;i<efidev.length() && efidev[i]!=' ';i++);
    efidev.resize(i);

    cout << "EFI device: " << efidev << "\n";

    int efipartition = efidev[efidev.length()-1]-'0'; // maximum 9 partitions are supported.
    string part = "";
    if (efipartition>=0 && efipartition<=9)
    {
        part = " --part ";
        part.push_back('0'+efipartition);
    }

    string mgrcommand = string("efibootmgr --create --disk ")+efidev+part+string(" --label \"") +bootOptionName+ "\" --loader \"" EFI_FILE_EFISTYLE  "\" --verbose";
    writeToFile(DATADIR "/boot_menu_installed_cmd",mgrcommand);
    execNoPipe(mgrcommand);
}

string findDevice()
{
    //    string res = exec("mount | grep \" on / type \"");
    string lsblk = exec("lsblk --fs");

    int i=0;
    string deviceuuid;
    string lastline="";

    while(i<lsblk.size())
    {
        string line;  line.clear();
        while(lsblk[i] && lsblk[i]!='\n')
        {
            line.push_back(lsblk[i]);
            i++;
        }
        if (lsblk[i]) i++;

        if (line[line.length()-1]=='/' && line[line.length()-2]==' ') // root line
        {
            int j = line.find("luks-");
            if (j!=-1)
            {
                j+=5;
                while(line[j] && line[j]!=' ')
                {
                    deviceuuid.push_back(line[j]);
                    j++;
                }
            }
        }
        lastline = line;
    }

    string device;
    i=0;
    while(i<lsblk.size())
    {
        string line;  line.clear();
        while(lsblk[i] && lsblk[i]!='\n')
        {
            line.push_back(lsblk[i]);
            i++;
        }
        if (lsblk[i]) i++;
        if (line.find(deviceuuid)!=-1 && line.find("crypto_LUKS")!=-1 && deviceuuid.length()>20 )
        {
            if (device.empty()==false) error("Failed to find block device containing LUKS volume");
            int j=0;
            while(j<line.length() && !(line[j]>='a' && line[j]<='z')) j++;
            while(j<line.length() && line[j]!=32)
            {
                device.push_back(line[j]);
                j++;
            }
        }
    }

    if (device.empty()) error("Failed to find block device containing LUKS volume");

    return string("/dev/") + device;

}

void addLuksKey()
{
    // Note: Manjaro installer adds two keys to the LUKS volume. 
    // In slot 1 there is this key file. It resides in the encrypted root partition
    // So that it will only prompt once for the password.
    // Unfortunately it's too big to be uploaded to the TPM2.0
    // but this key can be used to add more keys without prompting the user for password now

    if (fileExists(LUKSKEY)) return;

    string device = findDevice();
    cout << "Root partition device detected: " + device << "\n";

    string inputkey;
    if (optimizeLUKS)
    {
        string passwd = getpass("Please type your LUKS password:");
        toDelete = "/tmp/userpass.txt";
        writeToFile(toDelete,passwd);
        inputkey = "--key-file /tmp/userpass.txt ";
        cout << "verifying passphrase\n";

        string res = exec(string("cryptsetup luksOpen ")+device+" --test-passphrase --verbose --key-file /tmp/userpass.txt");
        if (res.find("successful")==-1) error("Wrong password supplied.");

        if (res.find("Key slot 0 unlocked")!=-1)
        {
            cout << "moving LUKS passphrase from slot 0 to another slot.\n";
            string res = exec(string("cryptsetup luksAddKey --verbose --key-file /tmp/userpass.txt ")+device+" /tmp/userpass.txt");
            if (true)
            {
                exec(string("cryptsetup luksKillSlot ")+device+" 0 --key-file /tmp/userpass.txt"); // making sure,  bytelocker key is placesd in slot 0
            }
        }
        
    }
    else
    {
        if (fileExists("/crypto_keyfile.bin")) inputkey = "--key-file /crypto_keyfile.bin ";
    }

    // note: high iteration count is recommended only for short and guessable passphrases 
    // to slow down brute force attaks. For 256bit keys it's unnecessary. (2.2*10^68 Years)
    cout << "adding extra luks key\n";
    execNoPipe("dd if=/dev/random of=" LUKSKEY " bs=32 count=1 > /dev/null 2>&1");
    filesystem::permissions(LUKSKEY,filesystem::perms::owner_read|filesystem::perms::owner_write,filesystem::perm_options::replace);
    string res;
    res = exec(string("cryptsetup luksAddKey --iter-time=60 ")+device+" "+inputkey+" " LUKSKEY);
    cout << res;
    
    if (toDelete.length()) filesystem::remove(toDelete);
}

void choseSHAalg()
{
    string res = exec("tpm2_pcrread");
    if (res.find("sha1:")==-1 || res.find("sha256:")==-1) error("Can't communicate with TPM2");
    shaalg = "sha1:";
    int i = res.find("sha256:");
    if (i>=0)
    {
        if (res.rfind("10: 0x")>i) // sha256 values found.
        {
            shaalg = "sha256:";
        }
    }
    if (shaalg.compare("sha256:")!=0) cout << "sha256 not supported by TPM hardware. Using sha1\n";

}

int main(int argc, char *argv[])
{
    if (argc<=1)
    {
        printUsage();
        return 0;
    }
    checkDeps();
    choseSHAalg();
    bootOptionName = getDistroName() + " Bytelocker";

    if (string("setup1").compare(argv[1])==0  )
    {
        string nvaddress;
        filesystem::create_directory(DATADIR);
        string imagefile = DATADIR "/initramfs-bytelocker.img";
        string kernel = findKernel();

        addLuksKey();

        nvaddress = findNVPersistentAddress(true);
        if (nvaddress.length()<3) error("Can't find available NVRAM persistent address");
        installHooks(nvaddress);
        genInitRamFS(imagefile);
        genEFI(kernel,imagefile);

        setBootManager();
        cout<<string("\nPlease restart your computer and select the \"") + bootOptionName+"\" option in the UEFI setup or BIOS\nYou will need to provide the passphrase one more time\nrun \"bytelocker setup2\" after reboot to finish setting up\n";
        return 0;
    }

    if (string("setup2").compare(argv[1])==0  )
    {
        addLuksKey();
        string nvaddress;
        nvaddress = findNVPersistentAddress(false);
        string newaddress = uploadToTPM(LUKSKEY,nvaddress );
        if (nvaddress.compare(newaddress)!=0) error("Uploading to TPM2 failed 2");
        string enckey = testunseal(nvaddress);
        string enckey2 = loadFile(LUKSKEY);
        if (enckey.compare(enckey2)!=0) error("Uploading to TPM2 failed");
        cout<<"Done.\n";
        return 0;
    }

    cout << "Unknown command\n";
    printUsage();
    return 0;
}
