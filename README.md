# qt-jpegxl-image-plugin

## Table of Contents

1. [Description](#Description)
2. [Installation](#Installation)

# Description

Qt plug-in to allow Qt and KDE based applications to read/write JXL images.

Work in progress experimental implementation using [libjxl](https://gitlab.com/wg1/jpeg-xl/)

# Installation

### 1. Clone, build and install JPEG-XL

**!Important!** Clone must be `--recursive` to include third party packages

Code for download and compilation:
```
git clone --depth 1 https://gitlab.com/wg1/jpeg-xl.git --recursive
cd jpeg-xl
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DJPEGXL_ENABLE_PLUGINS=ON -DBUILD_TESTING=OFF -DJPEGXL_WARNINGS_AS_ERRORS=OFF -DJPEGXL_ENABLE_SJPEG=OFF ..
make
```
Code for installation (run as root):

`make install`

### 2. Update mime database file

Check if the `image-jxl.xml` file was installed to `/usr/share/mime/packages/` folder and run (as root):

`update-mime-database /usr/share/mime/`

### 3. Build qt-jpegxl-image-plugin

Make sure that the Qt 5 base development packages (`qt5base-dev` on Debian/Ubuntu) are installed.

Download and build:
```
git clone --depth 1 https://github.com/novomesk/qt-jpegxl-image-plugin
cd qt-jpegxl-image-plugin
./build_libqjpegxl_dynamic.sh
```
Install (run as root):

`make install`

### 4. Add `image/jxl` to `/usr/share/kservices5/imagethumbnail.desktop`

1. Open `/usr/share/kservices5/imagethumbnail.desktop` with editor of your choise
2. Find line that starts with `MimeType=` and add `image/jxl;` to it's end, so line looks like `MimeType=image/cgm; ... ;image/rle;image/avif;image/jxl;`, save file.
3. Run `update-desktop-database`

# Test
```
cd testfiles
gwenview jpegxl-logo.jxl
```

Expected result:

![jpegxl-logo.jxl in gwenview](testfiles/gwenview.png)
