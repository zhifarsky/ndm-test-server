SourceDir=src
Source=$(SourceDir)/server.cpp

OutputDir=build
DebugOutputDir=$(OutputDir)/debug
ReleaseOutputDir=$(OutputDir)/release

PackageDir=package
PackageOutputDir=$(PackageDir)/usr/bin

OutputName=ndm-test-server

CC=g++

CommonFlags=-Wall
DebugFlags=$(CommonFlags) -g
ReleaseFlags=$(CommonFlags)

all: debug release

debug:
	mkdir -p $(DebugOutputDir)
	$(CC) $(Source) $(DebugFlags) -o $(DebugOutputDir)/$(OutputName)

release:
# компилируем
	mkdir -p $(ReleaseOutputDir)
	$(CC) $(Source) $(ReleaseFlags) -o $(ReleaseOutputDir)/$(OutputName)

# создаем пакет
	mkdir -p $(PackageOutputDir)
	cp $(ReleaseOutputDir)/$(OutputName) $(PackageOutputDir)
	dpkg-deb --build $(PackageDir) ndm-test-server_1.0_amd64.deb