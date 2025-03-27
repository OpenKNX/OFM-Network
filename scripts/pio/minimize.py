Import("env", "projenv")
import os
import gzip
import base64

class console_color:
    BLUE = '\033[94m'
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    END = '\033[0m'

print()
print("{}Minimizing files for Network-Webserver:{}".format(console_color.YELLOW, console_color.END))

webserver_base_uri = None
defines = projenv.get("CPPDEFINES", [])
for define in defines:
    if isinstance(define, tuple) and define[0] == "WEBSERVER_BASE_URI":
        webserver_base_uri = define[1]

if webserver_base_uri is None:
    with open("lib/OFM-Network/src/Webserver/Base_Webserver.h", "r") as f:
        content = f.read().splitlines()
        for line in content:
            if "#define WEBSERVER_BASE_URI" in line:
                webserver_base_uri = line.split("WEBSERVER_BASE_URI")[1].strip()
                break

if webserver_base_uri is None:
    webserver_base_uri = "/web"

print(f"  {console_color.BLUE}WEBSERVER_BASE_URI: {webserver_base_uri}{console_color.END}")

def minify_html(content):
    # Entfernt überflüssige Leerzeichen und Zeilenumbrüche
    return ' '.join(content.split())

def minify_js(content):
    # Entfernt Kommentare und überflüssige Leerzeichen
    lines = content.splitlines()
    minified = []
    for line in lines:
        if line:
            minified.append(line)
    return ''.join(minified)

def minify_css(content):
    # Entfernt Kommentare und überflüssige Leerzeichen
    content = ''.join(content.splitlines())  # Zeilenumbrüche entfernen
    return ''.join(part.split('/*')[0] for part in content.split('*/')).strip()  # Mehrzeilige Kommentare entfernen

def process_files(input_dir):
    if not os.path.exists(input_dir + "/www"):
        return

    print(("  {}" + input_dir + "{}").format(console_color.CYAN, console_color.END))

    file_dict = {}
    for root, _, files in os.walk(input_dir + "/www"):
        for file in files:
            file_path = os.path.join(root, file)
            with open(file_path, 'r') as f:
                content = f.read()
                if file.endswith('.html'):
                    minimized = minify_html(content)
                elif file.endswith('.js'):
                    minimized = minify_js(content)
                elif file.endswith('.css'):
                    minimized = minify_css(content)
                else:
                    continue
            
            size_old = os.path.getsize(file_path)

            with open(file_path, 'r') as f:
                content = f.read()

            if "#webserver#" in content:
                content = content.replace("#webserver#base-uri#", webserver_base_uri)
                with open(file_path + ".temp", 'w') as f:
                    f.write(content)
                file_path_temp = file_path + ".temp"
            else:
                file_path_temp = file_path

            with open(file_path_temp, 'rb') as f_in:
                compressed_data = gzip.compress(f_in.read())

            compressed_data = bytearray(compressed_data)
            # remove the timestamp
            compressed_data[4] = 0
            compressed_data[5] = 0
            compressed_data[6] = 0
            compressed_data[7] = 0

            size_new = len(compressed_data)
            print(f"    {os.path.basename(file_path):<30}: {round(size_new / size_old * 100, 2)}%   {size_old:>6} B -> {size_new:>6} B")
            variable_name = os.path.basename(file_path).replace(".", "_")
            output_file = input_dir + "/include/file_" + variable_name + ".h"

            if file_path_temp != file_path:
                os.remove(file_path_temp)

            # Headerdatei schreiben
            with open(output_file, 'w') as f_out:
                f_out.write(f"#ifndef FILE_{variable_name.upper()}_H\n")
                f_out.write(f"#define FILE_{variable_name.upper()}_H\n\n")
                f_out.write(f"static const uint8_t file_{variable_name}[] = {{\n")

                # Schreibe die Dateidaten als Hexadezimalwerte ins Array
                for i, byte in enumerate(compressed_data):
                    f_out.write(f"0x{byte:02x},")
                    if (i + 1) % 12 == 0:  # Zeilenumbruch nach 12 Werten
                        f_out.write("\n")
                f_out.write("\n};\n\n")
                f_out.write(f"static const unsigned int file_{variable_name}_len = {len(compressed_data)};\n")
                f_out.write(f"#endif // {variable_name.upper()}_H\n")


subfolders = [f.path for f in os.scandir("lib/") if f.is_dir()]
for subfolder in subfolders:
    process_files(subfolder)

print()