# qt-jpegxl-image-plugin

## Table of Contents

1. [Description](#Description)
2. [Installation](#Installation)

# Description

Qt plug-in to allow Qt and KDE based applications to read/write JXL images.

Work in progress experimental implementation using [libjxl](https://gitlab.com/wg1/jpeg-xl/)

# Installation

### 1. Clone and install JPEG XL

**!Important!** Clone must be `--recursive` to include third party packages

Code for installation:
```
git clone https://gitlab.com/wg1/jpeg-xl.git --recursive
cd jpeg-xl
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF ..
cmake --build . -- -j
sudo make install
```

### 2. Copy mime file

Copy `image-xl.xml` from `jpeg-xl/plugins/mime` to `/usr/share/mime/packages`

From `jpeg-xl/plugins/mime/`execute:

`sudo cp ./image-jxl.xml /usr/share/mime/packages`

and run:

`sudo update-mime-database /usr/share/mime `

### 3. Build qt-jpegxl-image-plugin

Clone, build and install:
```
git clone https://github.com/novomesk/qt-jpegxl-image-plugin
cd qt-jpegxl-image-plugin
./build_libqjpegxl_dynamic.sh
sudo make install
```

### 4. Add `image/jxl` to `/usr/share/kservices5/imagethumbnail.desktop`

1. Open `/usr/share/kservices5/imagethumbnail.desktop` with editor of choise
2. Find line that starts with `MimeType=` and add `image/jxl;` to it's end, so line looks like `MimeType=image/cgm; ... image/rle;image/jxl;`, save file.