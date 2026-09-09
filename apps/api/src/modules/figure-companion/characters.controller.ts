import { Controller, Get, UseGuards } from '@nestjs/common';
import { UserAuthGuard } from './auth.guards';
import { StoreService } from './store.service';

@Controller('characters')
@UseGuards(UserAuthGuard)
export class CharactersController {
  constructor(private readonly store: StoreService) {}

  @Get()
  listCharacters() {
    return this.store.listCharacters();
  }
}
