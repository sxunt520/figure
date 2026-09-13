import {
  Body,
  Controller,
  Delete,
  Get,
  Param,
  Patch,
  Post,
  Req,
  UseGuards,
} from '@nestjs/common';
import { UserAuthGuard, UserRequest } from './auth.guards';
import {
  BindDeviceDto,
  SpeakTextDto,
  UpdateCharacterDto,
  UpdateDeviceNameDto,
  UpdateVolumeDto,
} from './dto';
import { StoreService } from './store.service';

@Controller('devices')
@UseGuards(UserAuthGuard)
export class DevicesController {
  constructor(private readonly store: StoreService) {}

  @Get()
  listDevices(@Req() request: UserRequest) {
    return this.store.listDevices(request.user.id);
  }

  @Get(':deviceId')
  getDevice(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
  ) {
    return this.store.getDevice(request.user.id, deviceId);
  }

  @Post('bind')
  bindDevice(@Req() request: UserRequest, @Body() dto: BindDeviceDto) {
    return this.store.bindDevice(request.user.id, dto);
  }

  @Delete(':deviceId/binding')
  unbindDevice(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
  ) {
    return this.store.unbindDevice(request.user.id, deviceId);
  }

  @Patch(':deviceId/character')
  updateCharacter(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
    @Body() dto: UpdateCharacterDto,
  ) {
    return this.store.updateCharacter(
      request.user.id,
      deviceId,
      dto.characterId,
    );
  }

  @Patch(':deviceId/name')
  updateName(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
    @Body() dto: UpdateDeviceNameDto,
  ) {
    return this.store.updateDeviceName(request.user.id, deviceId, dto.name);
  }

  @Patch(':deviceId/volume')
  updateVolume(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
    @Body() dto: UpdateVolumeDto,
  ) {
    return this.store.updateVolume(request.user.id, deviceId, dto.volume);
  }

  @Post(':deviceId/speak')
  speakText(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
    @Body() dto: SpeakTextDto,
  ) {
    return this.store.speakText(request.user.id, deviceId, dto.text);
  }

  @Post(':deviceId/listen')
  startListening(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
  ) {
    return this.store.startListening(request.user.id, deviceId);
  }

  @Get(':deviceId/commands')
  listCommands(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
  ) {
    return this.store.listDeviceCommands(request.user.id, deviceId);
  }

  @Get(':deviceId/events')
  listEvents(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
  ) {
    return this.store.listDeviceEvents(request.user.id, deviceId);
  }

  @Get(':deviceId/messages')
  listMessages(
    @Req() request: UserRequest,
    @Param('deviceId') deviceId: string,
  ) {
    return this.store.listConversationMessages(request.user.id, deviceId);
  }
}
