import {
  Controller,
  Get,
  Header,
  Param,
  StreamableFile,
  UseGuards,
} from '@nestjs/common';
import { DeviceAuthGuard } from './auth.guards';
import { TtsService } from './tts.service';

@Controller('audio')
@UseGuards(DeviceAuthGuard)
export class AudioController {
  constructor(private readonly tts: TtsService) {}

  @Get(':fileName')
  @Header('Cache-Control', 'private, max-age=86400')
  async getAudio(@Param('fileName') fileName: string) {
    const audio = await this.tts.readCachedAudio(fileName);
    return new StreamableFile(audio.buffer, {
      type: 'audio/wav',
      length: audio.size,
    });
  }
}
