#include "core/AetherPDProtocol.h"
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <iostream>
int main(int argc,char**argv){
 QCoreApplication app(argc,argv); if(argc!=2)return 1;
 for(int spacing:{200,1001,1190,2610,10000}){
  QString dir=QString::fromLocal8Bit(argv[1])+QString("/tones-%1/source/").arg(spacing);
  QFile manifest(dir+"manifest.json");if(!manifest.open(QIODevice::ReadOnly))return 2;
  auto parsed=AetherSDR::aetherpd::parseCaptureManifest(manifest.readAll());
  if(!parsed || !parsed->feedbackBinding || !parsed->measurementToneConfig || parsed->measurementToneConfig->spacingHz!=quint32(spacing))return 3;
  const auto binding=*parsed->feedbackBinding;
  if(binding.channel!=3 || binding.pan!=0x40000002 || binding.guiHandle!=7 || !binding.valid || binding.failed)return 4;
  for(int i=0;i<32;++i){QFile chunk(dir+QString("chunk-%1.json").arg(i));if(!chunk.open(QIODevice::ReadOnly))return 5;
   if(!AetherSDR::aetherpd::parseCaptureChunk(chunk.readAll(),*parsed,i))return 6;}
 }
 std::cout<<"PASS: native C -> Go -> Qt binding/tone provenance, five spacings and 160 chunks\n";
}
