"""Copy firmware.bin ra mot thu muc dung chung sau moi lan build.

Ly do ton tai: ca 3 phong (can_tim / gia_sach / dat_the) deu dung cung ten env
"esp32-s3-devkitc-1", nen duong dan build cua chung GIONG HET NHAU:

    .pio/build/esp32-s3-devkitc-1/firmware.bin

Muon nap OTA tu link thi phai co URL rieng cho tung phong. Script nay copy ban vua
build sang <custom_fw_out>/<custom_fw_name>.bin, de mot HTTP server duy nhat phuc vu
ca fleet voi URL ngan va khong dung nhau:

    python -m http.server 8000 -d C:/fw
    -> http://<ip-may>:8000/cantim.bin
       http://<ip-may>:8000/giasach.bin
       http://<ip-may>:8000/datthe.bin

Cau hinh trong platformio.ini (bat buoc co custom_fw_name, khong thi script bo qua):

    extra_scripts = post:tools/copy_fw.py
    custom_fw_name = cantim
    custom_fw_out = C:/fw

File nay CO Y giong het nhau o ca 3 repo - moi thu khac biet nam trong platformio.ini.
Sua o day thi nho chep sang 2 repo con lai.
"""

Import("env")

import os
import shutil


def copy_firmware(source, target, env):
    name = env.GetProjectOption("custom_fw_name", "")
    if not name:
        print("copy_fw: thieu custom_fw_name trong platformio.ini - bo qua buoc copy")
        return

    out_dir = env.GetProjectOption("custom_fw_out", "C:/fw")
    src = os.path.join(env.subst("$BUILD_DIR"), "firmware.bin")
    dst = os.path.join(out_dir, name + ".bin")

    # Loi copy KHONG duoc lam fail ca build: thu muc dich nam ngoai repo (o dia may ca
    # nhan), co the chua tao, bi khoa, hoac nam tren o mang dang rot. Build da xong roi -
    # bao warning de nguoi dung biet URL van con tro toi ban CU, roi di tiep.
    try:
        os.makedirs(out_dir, exist_ok=True)
        shutil.copyfile(src, dst)
        print("copy_fw: %s -> %s (%d bytes)" % (name, dst, os.path.getsize(dst)))
    except Exception as e:
        print("copy_fw: CANH BAO - khong copy duoc sang %s (%s)" % (dst, e))
        print("copy_fw: URL nap tu link van dang tro toi ban firmware CU")


# PROGNAME mac dinh la "firmware" -> chinh la $BUILD_DIR/firmware.bin.
# LUU Y: SCons chi chay post-action khi target that su duoc build lai. Build ma khong co
# gi thay doi thi buoc copy nay khong chay - neu vua xoa file trong thu muc dich thi phai
# sua mot file nguon (hoac chay "pio run -t clean") de ep build lai.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)
