import pefile
pe=pefile.PE(r"E:/SteamLibrary/steamapps/common/Kenshi/kenshi_x64.exe",fast_load=True)
print(pe.get_data(0x7874E0,28).hex(' '))
