import { Module } from '@nestjs/common';
import { TypeOrmModule } from '@nestjs/typeorm';
import { DeviceAuthGuard, UserAuthGuard } from './auth.guards';
import { AuthController } from './auth.controller';
import { CharactersController } from './characters.controller';
import { DevicesController } from './devices.controller';
import { databaseEntities } from './entities';
import { HardwareController } from './hardware.controller';
import { RemindersController } from './reminders.controller';
import { StoreService } from './store.service';

@Module({
  imports: [TypeOrmModule.forFeature(databaseEntities)],
  controllers: [
    AuthController,
    CharactersController,
    DevicesController,
    RemindersController,
    HardwareController,
  ],
  providers: [StoreService, UserAuthGuard, DeviceAuthGuard],
})
export class FigureCompanionModule {}
