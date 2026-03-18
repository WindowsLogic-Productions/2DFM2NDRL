// Character State Machine: Massive 8KB function handling all character behavior and animation.
void character_state_machine(void)
{
  int v0; // esi
  int *object_data_ptr; // ebx
  int character_data_ptr; // eax
  int state_value; // eax
  bool initialization_flag; // zf
  bool game_mode_value; // cc
  int zero_flag; // ecx
  int less_equal_flag; // ecx
  int character_index; // eax
  _DWORD *temp_index; // edi
  char player_index; // al
  char stage_script_ptr; // cl
  unsigned int script_flag; // eax
  unsigned int script_value; // ecx
  unsigned int calculated_hp; // eax
  int hp_temp; // eax
  int max_hp_value; // edi
  int flags_value; // ecx
  int script_entry_ptr; // eax
  int position_y; // eax
  int animation_index; // edx
  int animation_entry_ptr; // edi
  int selected_action; // ecx
  int action_conflict_flag; // eax
  int action_iterator; // edx
  int action_ptr; // eax
  int demo_hp; // eax
  _DWORD *game_object; // eax
  int animation_data_ptr; // ecx
  int v29; // edx
  int current_object_data; // eax
  int player_id; // eax
  int updated_object_data; // ecx
  int object_state; // edi
  int sprite_base_ptr; // ebp
  int sprite_offset; // eax
  unsigned int frame_counter; // edx
  int *sprite_y_pos; // eax
  int sprite_data_ptr; // edi
  int hit_priority; // eax
  int attack_state_id; // eax
  int delay_timer; // eax
  int object_type; // eax
  _DWORD *character_offset; // eax
  int next_attack_state; // eax
  int parent_object_ptr; // eax
  int position_x; // eax
  int boundary_check; // eax
  int saved_attack_state; // eax
  int state_flags; // ecx
  int position_x_check; // eax
  int position_y_check; // edx
  int timer_value; // eax
  int timer_remaining; // eax
  int position_y_temp; // ebp
  int animation_timer; // eax
  int timer_decremented; // edx
  int loop_counter; // eax
  int reference_ptr; // edx
  int reference_count; // edi
  int current_attack_state; // eax
  int script_command_ptr; // eax
  char saved_attack_state_id; // al
  int saved_timer; // edx
  int timer_decrement; // eax
  int sprite_data_base; // eax
  int saved_attack_state_high; // eax
  int saved_attack_state_low; // edx
  unsigned __int16 previous_attack_state; // di
  int sprite_data_base_ptr; // eax
  int delay_frames; // ebp
  int delay_result; // edx
  char velocity_absolute_flag; // cl
  int velocity_direction; // eax
  int command_flags; // eax
  int velocity_x_calc; // eax
  int velocity_y_calc; // eax
  int velocity_x_value; // eax
  int velocity_y_value; // edx
  int jump_target_value; // eax
  int sprite_data_base_temp; // ecx
  unsigned __int16 attack_state_id_temp; // ax
  int sprite_offset_calc; // ecx
  unsigned __int16 animation_frame_count; // ax
  int frame_offset; // edx
  int saved_attack_state_temp; // ecx
  int sprite_data_base_temp2; // ecx
  int frame_offset_temp; // eax
  int animation_frame_count_temp; // eax
  _DWORD *attack_state_id_temp2; // edx
  _DWORD *object_slot_index; // ebp
  int object_pool_ptr; // eax
  _DWORD *object_slot_ptr; // ecx
  int object_index; // edx
  int object_pool_iterator; // eax
  int sprite_data_base_ptr2; // ecx
  int attack_state_id_ptr; // eax
  int frame_offset_ptr; // ebp
  int spawn_x_pos; // ecx
  int spawn_y_pos; // eax
  _DWORD *hit_priority_value; // eax
  int spawn_x_offset; // edx
  unsigned __int16 spawned_object_ptr; // cx
  char facing_direction; // cl
  int sprite_id; // ecx
  int *spawn_flags; // eax
  int object_flags; // eax
  int reference_slot_ptr; // eax
  char reference_flags; // cl
  int state_flags_temp; // eax
  int input_check_flag; // ecx
  int player_id_temp; // eax
  _DWORD *input_history_value; // ebp
  char target_attack_state; // al
  int hitbox_object_ptr; // edx
  int hitbox_flags; // ecx
  int hit_priority_self; // edx
  char hit_priority_other; // cl
  int hit_priority_target; // ecx
  int facing_direction_temp; // ecx
  unsigned __int16 hitbox_x_offset; // cx
  int hitbox_object_state; // eax
  int hitbox_sprite_id; // esi
  int hitbox_character_offset; // ecx
  int hitbox_sprite_data_ptr; // eax
  __int16 hitbox_sprite_id_temp; // dx
  int hitbox_character_offset_temp; // ecx
  int hitbox_damage; // edx
  int hitbox_x_offset_temp; // eax
  _DWORD *hitbox_y_offset; // ebp
  char hitbox_state_flags; // al
  int hitbox_object_ptr2; // edx
  int hitbox_flags2; // ecx
  int hit_priority_self2; // edx
  char hit_priority_other2; // cl
  int hit_priority_target2; // ecx
  int facing_direction_temp2; // eax
  int hitbox_x_offset2; // edx
  unsigned __int16 hitbox_state_flags2; // ax
  int sprite_data_base_ptr3; // eax
  int target_attack_state_id; // edx
  int ai_result; // eax
  char *sprite_data_base_ptr4; // ebp
  int animation_frame_count_result; // eax
  int character_data_ptr2; // esi
  int hitbox_object_ptr3; // ebp
  char *hitbox_player_id; // esi
  unsigned __int8 hitbox_object_ptr4; // dl
  _DWORD *target_character_data_ptr; // eax
  int effect_id; // ecx
  int effect_data_ptr; // eax
  _DWORD *effect_param1; // eax
  int hitbox_object_ptr5; // ecx
  char effect_data_ptr2; // cl
  int effect_param6; // eax
  int effect_flags; // ecx
  int shake_effect_param1; // eax
  int shake_effect_param2; // edx
  int shake_effect_param3; // eax
  unsigned __int8 shake_effect_param4; // al
  int shake_effect_param5; // esi
  int shake_effect_id2; // eax
  int character_offset_temp; // ecx
  unsigned int char_value_result; // eax
  unsigned __int16 char_value_current; // ax
  int char_value_max; // eax
  unsigned __int16 target_attack_state_id2; // ax
  int ai_result2; // ecx
  int animation_frame_count_result2; // eax
  int sprite_offset_calc2; // ecx
  int target_attack_state_id3; // edx
  int shake_effect_param6; // eax
  _DWORD *player_id_temp2; // eax
  int character_offset_temp2; // ebp
  unsigned __int8 object_pool_iterator2; // cl
  int object_index2; // edi
  int delay_frames2; // eax
  _DWORD *character_data_iterator; // eax
  __int16 object_ptr_temp; // dx
  unsigned int object_ptr_temp2; // eax
  __int16 *variable_id; // ebp
  unsigned int variable_value; // eax
  int variable_ptr; // edx
  int source_variable_id; // eax
  unsigned int variable_value_result; // ecx
  int position_value; // eax
  int operation_flags; // ecx
  int variable_value_added; // ecx
  int comparison_type; // edx
  int comparison_type_sub; // eax
  int sprite_data_base_ptr5; // ecx
  int target_attack_state_id4; // edx
  int frame_offset_result; // eax
  int sprite_data_base_ptr6; // ecx
  int target_attack_state_id5; // ecx
  int frame_offset_result2; // edx
  int sprite_param1; // eax
  unsigned __int16 sprite_param2; // ax
  unsigned __int16 sprite_param3; // cx
  int target_attack_state_id6; // edx
  int animation_frame_count_result3; // eax
  int sprite_data_base_ptr7; // ecx
  int sprite_index; // eax
  int *state_flags_temp2; // ecx
  int sprite_data_index; // edx
  int sprite_data_array_ptr; // [esp+10h] [ebp-11Ch]
  int *sprite_data_array_index; // [esp+18h] [ebp-114h]
  char variable_value_result2; // [esp+18h] [ebp-114h]
  int selected_action_ptr; // [esp+18h] [ebp-114h]
  int spawn_flag_value; // [esp+18h] [ebp-114h]
  int damage_value; // [esp+1Ch] [ebp-110h]
  int object_pool_loop_count; // [esp+1Ch] [ebp-110h]
  int action_conflict_check; // [esp+20h] [ebp-10Ch]
  int character_data_iterator2; // [esp+24h] [ebp-108h]
  int *variable_ptr_result; // [esp+28h] [ebp-104h]
  unsigned __int8 loop_continue_flag; // [esp+28h] [ebp-104h]
  char Buffer[256]; // [esp+2Ch] [ebp-100h] BYREF

  v0 = g_object_data_ptr;
  switch ( *(_DWORD *)(g_object_data_ptr + 346) )
  {
    case 0:
    case 1:
    case 5:
      object_data_ptr = (int *)((char *)&g_character_data_base + 57407 * *(_DWORD *)(g_object_data_ptr + 342));
      break;
    case 2:
      object_data_ptr = g_projectile_character_data_base;
      break;
    case 3:
      object_data_ptr = g_effect_character_data_base;
      break;
    case 4:
      object_data_ptr = g_special_object_character_data_base;
      break;
    default:
      object_data_ptr = variable_ptr_result;
      break;
  }
  character_data_ptr = *(_DWORD *)(g_object_data_ptr + 338);
  if ( character_data_ptr )
  {
    if ( character_data_ptr != 1 )
      return;
  }
  else
  {
    *(_DWORD *)(g_object_data_ptr + 338) = 1;
    switch ( *(_DWORD *)(v0 + 346) )
    {
      case 0:
        state_value = g_game_mode_flag;
        initialization_flag = g_game_mode_flag == 0;
        game_mode_value = g_game_mode_flag <= 0;
        *(_DWORD *)(v0 + 4) = g_hit_priority_table[*(_DWORD *)(v0 + 20) & 1];
        if ( initialization_flag )
        {
          character_index = *(_DWORD *)(v0 + 342);
          temp_index = (_DWORD *)(206 * g_stage_script_index[g_css_active_player] + 5085769);
          if ( character_index )
          {
            *(int *)((char *)object_data_ptr + 57093) = *(int *)((char *)object_data_ptr + 31914);
            max_hp_value = (int)temp_index + 26 * character_index - 2;
            *(int *)((char *)object_data_ptr + 57101) = *(_DWORD *)max_hp_value & 1;
            *(int *)((char *)object_data_ptr + 57271) = *(unsigned __int8 *)(max_hp_value + 6);
            if ( ((*(_DWORD *)max_hp_value >> 1) & 3) == 1 )
            {
              *(_DWORD *)(v0 + 64) = 100 * *(unsigned __int8 *)(max_hp_value + 11);
              if ( *(_BYTE *)(max_hp_value + 12) )
              {
                *(_DWORD *)(v0 + 64) += 100 * (game_rand() % *(unsigned __int8 *)(max_hp_value + 12));
                v0 = g_object_data_ptr;
              }
            }
            else if ( ((*(_DWORD *)max_hp_value >> 1) & 3) == 2
                   && *(int *)((char *)&g_p1_hp + 57407 * *(unsigned __int8 *)(max_hp_value + 13)) >= *(unsigned __int8 *)(max_hp_value + 14) )
            {
              *(_DWORD *)(v0 + 338) = 0;
              return;
            }
            *(int *)((char *)object_data_ptr + 57355) = -1;
            *(int *)((char *)object_data_ptr + 57109) = *((char *)object_data_ptr + 31938);
            *(int *)((char *)object_data_ptr + 57117) = 0;
            if ( (*(_DWORD *)max_hp_value & 0x200) == 0 )
              *(_DWORD *)(v0 + 92) = 1;
          }
          else
          {
            player_index = *((_BYTE *)&g_stage_script_flag_array + 206 * g_stage_script_index[g_css_active_player]);
            if ( g_game_timer == 1 )
            {
              if ( !player_index )
                *(int *)((char *)object_data_ptr + 57093) = *(int *)((char *)object_data_ptr + 31914);
            }
            else if ( player_index )
            {
              stage_script_ptr = *((_BYTE *)&g_stage_script_hp_percent_array
                                 + 206 * g_stage_script_index[g_css_active_player]);
              if ( stage_script_ptr == 100 )
              {
                *(int *)((char *)object_data_ptr + 57093) = *(int *)((char *)object_data_ptr + 31914);
              }
              else if ( stage_script_ptr )
              {
                script_flag = *(int *)((char *)object_data_ptr + 31914)
                            * (unsigned int)*((unsigned __int8 *)&g_stage_script_hp_percent_array
                                            + 206 * g_stage_script_index[g_css_active_player])
                            / 0x64
                            + *(int *)((char *)object_data_ptr + 57093);
                *(int *)((char *)object_data_ptr + 57093) = script_flag;
                script_value = script_flag;
                calculated_hp = *(int *)((char *)object_data_ptr + 31914);
                if ( script_value > calculated_hp )
                  *(int *)((char *)object_data_ptr + 57093) = calculated_hp;
              }
            }
            else
            {
              *(int *)((char *)object_data_ptr + 57093) = *(int *)((char *)object_data_ptr + 31914);
            }
            *(int *)((char *)object_data_ptr + 57271) = -2;
            if ( (temp_index[6] & 0x200) != 0 )
              *(int *)((char *)object_data_ptr + 57271) = -4;
            if ( (*(_DWORD *)((char *)temp_index + 50) & 0x200) != 0 )
              *(int *)((char *)object_data_ptr + 57271) &= ~4u;
            if ( (temp_index[19] & 0x200) != 0 )
              *(int *)((char *)object_data_ptr + 57271) &= ~8u;
            if ( (*(_DWORD *)((char *)temp_index + 102) & 0x200) != 0 )
              *(int *)((char *)object_data_ptr + 57271) &= ~0x10u;
            if ( (temp_index[32] & 0x200) != 0 )
              *(int *)((char *)object_data_ptr + 57271) &= ~0x20u;
            if ( (*(_DWORD *)((char *)temp_index + 154) & 0x200) != 0 )
              *(int *)((char *)object_data_ptr + 57271) &= ~0x40u;
            if ( (temp_index[45] & 0x200) != 0 )
            {
              hp_temp = *(int *)((char *)object_data_ptr + 57271);
              LOBYTE(hp_temp) = hp_temp & 0x7F;
              *(int *)((char *)object_data_ptr + 57271) = hp_temp;
            }
            *(int *)((char *)object_data_ptr + 57101) = 1;
          }
          *(_DWORD *)(v0 + 96) = *(_DWORD *)(v0 + 342);
        }
        else if ( !game_mode_value && state_value <= 2 )
        {
          zero_flag = *(_DWORD *)(v0 + 342);
          *(int *)((char *)object_data_ptr + 57093) = *(int *)((char *)object_data_ptr + 31914);
          *(_DWORD *)(v0 + 96) = zero_flag;
          less_equal_flag = *(_DWORD *)(v0 + 342);
          *(int *)((char *)object_data_ptr + 57271) = -1 - (1 << less_equal_flag);
          if ( less_equal_flag )
          {
            *(int *)((char *)object_data_ptr + 57165) = 1;
            *(_DWORD *)(v0 + 92) = 1;
          }
          if ( *(_DWORD *)(v0 + 342) )
          {
            *(_DWORD *)(v0 + 8) = 58327040;
            *(int *)((char *)object_data_ptr + 57153) = 58327040;
          }
          else
          {
            *(_DWORD *)(v0 + 8) = 25559040;
            *(int *)((char *)object_data_ptr + 57153) = 25559040;
          }
        }
        flags_value = *(_DWORD *)(v0 + 12);
        *(int *)((char *)object_data_ptr + 57153) = *(_DWORD *)(v0 + 8);
        *(int *)((char *)object_data_ptr + 57157) = flags_value;
        *(int *)((char *)object_data_ptr + 57105) = *(int *)((char *)object_data_ptr + 31914);
        game_mode_value = g_game_timer < 2;
        *(int *)((char *)object_data_ptr + 57089) = 0;
        if ( game_mode_value )
        {
          *(int *)((char *)object_data_ptr + 57109) = *((char *)object_data_ptr + 31938);
          *(int *)((char *)object_data_ptr + 57117) = 0;
        }
        *(int *)((char *)object_data_ptr + 57113) = *(int *)((char *)object_data_ptr + 31922);
        *(int *)((char *)object_data_ptr + 57121) = *(int *)((char *)object_data_ptr + 31918);
        *(int *)((char *)object_data_ptr + 57125) = 0;
        *(int *)((char *)object_data_ptr + 57129) = 0;
        *(int *)((char *)object_data_ptr + 57077) = v0;
        *(int *)((char *)object_data_ptr + 57081) = 0;
        *(int *)((char *)object_data_ptr + 57193) = 0;
        *(int *)((char *)object_data_ptr + 57197) = 0;
        *(int *)((char *)object_data_ptr + 57201) = 0;
        *(int *)((char *)object_data_ptr + 57205) = 0;
        *(int *)((char *)object_data_ptr + 57209) = 0;
        *(int *)((char *)object_data_ptr + 57213) = 0;
        *(int *)((char *)object_data_ptr + 57141) = 20;
        object_data_ptr[2190] = 1;
        *(int *)((char *)object_data_ptr + 57173) = 0;
        *(int *)((char *)object_data_ptr + 57327) = 0;
        *(int *)((char *)object_data_ptr + 57351) = 0;
        *(int *)((char *)object_data_ptr + 57161) = 0;
        *(int *)((char *)object_data_ptr + 57085) = 0;
        *(int *)((char *)object_data_ptr + 57359) = 0;
        *(int *)((char *)object_data_ptr + 57275) = 0;
        *(int *)((char *)object_data_ptr + 57319) = 0;
        *(int *)((char *)object_data_ptr + 57323) = 0;
        script_entry_ptr = 39 * *((unsigned __int16 *)object_data_ptr + 15047) + object_data_ptr[68];
        if ( *(unsigned __int16 *)(script_entry_ptr + 71) > *(unsigned __int16 *)(script_entry_ptr + 32) + 1 )
          *(int *)((char *)object_data_ptr + 57319) = 1;
        position_y = 39 * *((unsigned __int16 *)object_data_ptr + 15048) + object_data_ptr[68];
        if ( *(unsigned __int16 *)(position_y + 71) > *(unsigned __int16 *)(position_y + 32) + 1 )
          *(int *)((char *)object_data_ptr + 57323) = 1;
        sprite_data_array_index = (int *)((char *)&g_char_selected_action + 57407 * *(_DWORD *)(v0 + 342));
        animation_index = *sprite_data_array_index;
        if ( *sprite_data_array_index == -1 )
          animation_index = 0;
        action_conflict_check = 0;
        break;
      case 1:
        *(_DWORD *)(v0 + 96) = *(_DWORD *)(v0 + 342);
        goto LABEL_82;
      case 4:
        *(_DWORD *)(v0 + 100) = *(_DWORD *)(v0 + 48);
        *(_DWORD *)(v0 + 104) = *(_DWORD *)(v0 + 44);
        goto LABEL_82;
      case 5:
        *(_DWORD *)(v0 + 40) |= 0x40000000u;
        goto LABEL_82;
      default:
        goto LABEL_82;
    }
    while ( 1 )
    {
      animation_entry_ptr = 0;
      selected_action = 0;
      action_conflict_flag = (int)&g_char_selected_action;
      do
      {
        if ( *(_DWORD *)(v0 + 342) != selected_action && animation_index == *(_DWORD *)action_conflict_flag )
          animation_entry_ptr = 1;
        action_conflict_flag += 57407;
        ++selected_action;
      }
      while ( action_conflict_flag < 5570435 );
      if ( !animation_entry_ptr )
        break;
      animation_index = (animation_index + 1) % 8;
      if ( ++action_conflict_check >= 8 )
        goto LABEL_72;
    }
    *sprite_data_array_index = animation_index;
LABEL_72:
    if ( g_game_mode_flag == 2 && g_demo_mode_player_id == *(_DWORD *)(v0 + 342) )
    {
      action_iterator = g_demo_mode_attack_state;
      action_ptr = g_demo_mode_frame;
      *(int *)((char *)object_data_ptr + 57093) = g_demo_mode_hp;
      *(int *)((char *)object_data_ptr + 57109) = action_iterator;
      *(int *)((char *)object_data_ptr + 57117) = action_ptr;
    }
    demo_hp = 39 * *((unsigned __int16 *)object_data_ptr + 15060) + object_data_ptr[68];
    if ( *(unsigned __int16 *)(demo_hp + 71) <= *(unsigned __int16 *)(demo_hp + 32) + 1 )
    {
      *(_DWORD *)(v0 + 40) |= 0x80000000;
    }
    else
    {
      game_object = create_game_object(*(_DWORD *)v0, 13, 0, 0);
      animation_data_ptr = g_object_data_ptr;
      *(_DWORD *)((char *)game_object + 346) = 5;
      v29 = *(_DWORD *)(animation_data_ptr + 342);
      LOWORD(animation_data_ptr) = *((_WORD *)object_data_ptr + 15060);
      *(_DWORD *)((char *)game_object + 342) = v29;
      game_object[12] = (unsigned __int16)animation_data_ptr;
      game_object[11] = *(unsigned __int16 *)(39 * (unsigned __int16)animation_data_ptr + object_data_ptr[68] + 32);
    }
    SetCharacterAttackState(*((unsigned __int16 *)object_data_ptr + 15054));
    current_object_data = g_object_data_ptr;
    *(_DWORD *)(g_object_data_ptr + 350) = *(_DWORD *)(g_object_data_ptr + 350) & 0xFFFFFFF3 | 4;
    memory_clear(&g_p1_input_history[1024 * *(_DWORD *)(current_object_data + 342)], 0x1000u);
    memory_clear((char *)object_data_ptr + 57233, 6u);
    memory_clear((char *)object_data_ptr + 57239, 0x20u);
    memory_clear((char *)object_data_ptr + 57363, 0x2Cu);
    v0 = g_object_data_ptr;
    *(_DWORD *)(g_object_data_ptr + 40) |= 0x40000000u;
    memset((char *)object_data_ptr + 57279, 0, 0x28u);
LABEL_82:
    *(_DWORD *)(v0 + 52) = *(_DWORD *)(v0 + 48);
  }
  if ( (*(_BYTE *)(v0 + 20) & 1) != 0 )
    *(_DWORD *)(v0 + 88) = 55705600;
  else
    *(_DWORD *)(v0 + 88) = 60293120;
  if ( g_game_paused )
  {
    player_id = *(_DWORD *)(v0 + 346);
    if ( player_id >= 0 && (player_id <= 1 || player_id == 5) )
      return;
  }
  if ( *(_BYTE *)(v0 + 337) )
  {
    updated_object_data = 404 * *(unsigned __int8 *)(v0 + 337);
    if ( *(_BYTE *)(g_sprite_data_array[updated_object_data] + 4) )
    {
      object_state = g_sprite_animation_frame[404 * *(unsigned __int8 *)(v0 + 337)];
      sprite_base_ptr = 16 * *(_DWORD *)(v0 + 44) + object_data_ptr[69] - 16;
      sprite_offset = g_sprite_frame_counter[404 * *(unsigned __int8 *)(v0 + 337)] - 1;
      g_sprite_frame_counter[404 * *(unsigned __int8 *)(v0 + 337)] = sprite_offset;
      if ( sprite_offset < 0 )
      {
        g_sprite_frame_counter[updated_object_data] = *(unsigned __int8 *)(g_sprite_data_array[updated_object_data] + 4);
        g_sprite_position_data[4 * object_state + 4 + updated_object_data] = *(_DWORD *)(v0 + 8);
        frame_counter = *(_DWORD *)(v0 + 12);
        sprite_y_pos = &g_sprite_position_data[4 * object_state + updated_object_data];
        sprite_data_ptr = *(_DWORD *)(v0 + 92);
        sprite_y_pos[5] = frame_counter;
        LOWORD(frame_counter) = *(_WORD *)(sprite_base_ptr + 3);
        sprite_y_pos[6] = ((frame_counter >> 14) & 1) + 4 * sprite_data_ptr;
        sprite_y_pos[7] = sprite_base_ptr;
        g_sprite_animation_frame[updated_object_data] = (g_sprite_animation_frame[updated_object_data] + 1) % 100;
      }
    }
  }
  if ( *(_DWORD *)(v0 + 56) )
  {
    *(_DWORD *)(v0 + 48) = -1;
    ChangeCharacterAttackState((unsigned __int16)*(_DWORD *)(v0 + 56));
    hit_priority = g_object_data_ptr;
    *(_DWORD *)(g_object_data_ptr + 44) += HIWORD(*(_DWORD *)(g_object_data_ptr + 56));
    *(_DWORD *)(hit_priority + 56) = 0;
    *(_BYTE *)(hit_priority + 124) = 0;
    ResetObjectVelocity();
    ClearObjectFlag297(g_object_data_ptr);
    ClearObjectHitboxData(g_object_data_ptr);
    v0 = g_object_data_ptr;
    goto LABEL_125;
  }
  attack_state_id = *(_DWORD *)(v0 + 64);
  if ( attack_state_id )
  {
    if ( attack_state_id != -1 )
      *(_DWORD *)(v0 + 64) = attack_state_id - 1;
    return;
  }
  delay_timer = *(_DWORD *)(v0 + 346);
  if ( !delay_timer )
  {
    *(int *)((char *)&g_input_override_active + 57407 * *(_DWORD *)(v0 + 342)) = 0;
    character_facing_controller();
    character_action_controller();
    state_flags = *(int *)((char *)object_data_ptr + 57125);
    if ( state_flags )
    {
      position_x_check = *(int *)((char *)object_data_ptr + 57129);
      if ( position_x_check )
        *(int *)((char *)object_data_ptr + 57129) = position_x_check - 1;
      else
        *(int *)((char *)object_data_ptr + 57125) = state_flags - 1;
    }
    v0 = g_object_data_ptr;
    position_y_check = *(_DWORD *)(g_object_data_ptr + 12);
    *(int *)((char *)object_data_ptr + 57153) = *(_DWORD *)(g_object_data_ptr + 8);
    *(int *)((char *)object_data_ptr + 57157) = position_y_check;
    goto LABEL_125;
  }
  object_type = delay_timer - 1;
  if ( object_type )
  {
    if ( object_type == 4 )
    {
      character_offset = *(_DWORD **)(v0 + 378);
      if ( *character_offset == 1 )
      {
LABEL_450:
        ClearObjectReferences();
        return;
      }
      if ( character_offset[4] != -1 )
        return;
      *(_DWORD *)(v0 + 8) = character_offset[2];
      *(_DWORD *)(v0 + 12) = *(_DWORD *)(*(_DWORD *)(v0 + 378) + 88);
      *(_DWORD *)(v0 + 92) = *(_DWORD *)(*(_DWORD *)(v0 + 378) + 92);
    }
    goto LABEL_125;
  }
  next_attack_state = *(_DWORD *)(v0 + 88);
  if ( *(_DWORD *)(v0 + 12) >= next_attack_state && *(int *)(v0 + 28) > 0 )
  {
    *(_DWORD *)(v0 + 12) = next_attack_state;
    *(_DWORD *)(v0 + 32) = 0;
    *(_DWORD *)(v0 + 24) = 0;
    *(_DWORD *)(v0 + 36) = 0;
    *(_DWORD *)(v0 + 28) = 0;
    *(_DWORD *)(v0 + 350) &= 0xFFFFFFFC;
    *(_DWORD *)(v0 + 48) = -1;
    if ( *(_DWORD *)(v0 + 100) )
    {
      parent_object_ptr = (unsigned __int16)*(_DWORD *)(v0 + 100);
      *(_DWORD *)(v0 + 48) = parent_object_ptr;
      *(_DWORD *)(v0 + 44) = HIWORD(*(_DWORD *)(v0 + 100))
                           + *(unsigned __int16 *)(*(int *)((char *)&g_character_sprite_data
                                                          + 57407 * *(_DWORD *)(v0 + 342))
                                                 + 39 * parent_object_ptr
                                                 + 32);
      *(_DWORD *)(v0 + 100) = 0;
      *(_DWORD *)(v0 + 60) = 0;
      ClearObjectHitboxData(v0);
      v0 = g_object_data_ptr;
    }
    else
    {
      UpdateCharacterFacing();
      v0 = g_object_data_ptr;
      position_x = *(_DWORD *)(g_object_data_ptr + 350);
      LOBYTE(position_x) = position_x & 0xE3;
      *(_DWORD *)(g_object_data_ptr + 350) = position_x;
    }
  }
  boundary_check = *(_DWORD *)(v0 + 8);
  if ( boundary_check >= -3276800 && boundary_check <= 87162880 )
  {
    saved_attack_state = *(_DWORD *)(v0 + 12);
    if ( saved_attack_state >= -3276800 && saved_attack_state <= 66191360 )
      goto LABEL_117;
  }
  if ( (*(_DWORD *)(v0 + 40) & 0x20000000) == 0 )
  {
    ClearObjectReferences();
    v0 = g_object_data_ptr;
LABEL_117:
    if ( (*(_DWORD *)(v0 + 40) & 0x20000000) == 0 )
      goto LABEL_125;
  }
  if ( **(_DWORD **)(v0 + 378) == 1 )
  {
    ClearObjectReferences();
    v0 = g_object_data_ptr;
  }
LABEL_125:
  timer_value = *(_DWORD *)(v0 + 60);
  if ( timer_value >= 0 )
  {
    timer_remaining = timer_value - 100;
    *(_DWORD *)(v0 + 60) = timer_remaining;
    if ( timer_remaining < 0 )
    {
      position_y_temp = 1;
      animation_timer = v0 + 213;
      timer_decremented = 20;
      do
      {
        if ( *(_DWORD *)animation_timer && (*(_BYTE *)(*(_DWORD *)animation_timer + 10) & 2) != 0 )
          *(_DWORD *)(v0 + 350) &= ~0x10u;
        --timer_decremented;
        animation_timer -= 4;
      }
      while ( timer_decremented );
      while ( 1 )
      {
        loop_counter = *(_DWORD *)(v0 + 48);
        character_data_iterator2 = position_y_temp + 1;
        if ( position_y_temp + 1 > 300 )
          break;
        if ( *(unsigned __int16 *)(39 * loop_counter + object_data_ptr[68] + 71) <= *(int *)(v0 + 44) )
        {
LABEL_139:
          if ( *(_DWORD *)(v0 + 133) )
          {
            current_attack_state = (unsigned __int16)*(_DWORD *)(v0 + 133);
            *(_DWORD *)(v0 + 48) = current_attack_state;
            *(_DWORD *)(v0 + 44) = *(unsigned __int16 *)(39 * current_attack_state + object_data_ptr[68] + 32)
                                 + (*(int *)(v0 + 133) >> 16)
                                 + 1;
            *(_DWORD *)(v0 + 133) = 0;
          }
          else
          {
            script_command_ptr = *(unsigned __int8 *)(v0 + 124);
            if ( (_BYTE)script_command_ptr )
            {
              saved_attack_state_id = script_command_ptr - 1;
              *(_BYTE *)(v0 + 124) = saved_attack_state_id;
              saved_timer = object_data_ptr[68];
              if ( saved_attack_state_id )
              {
                sprite_data_base = (unsigned __int16)*(_DWORD *)(v0 + 125);
                *(_DWORD *)(v0 + 48) = sprite_data_base;
                *(_DWORD *)(v0 + 44) = (*(int *)(v0 + 125) >> 16)
                                     + *(unsigned __int16 *)(39 * sprite_data_base + saved_timer + 32);
              }
              else
              {
                timer_decrement = (unsigned __int16)*(_DWORD *)(v0 + 129);
                *(_DWORD *)(v0 + 48) = timer_decrement;
                *(_DWORD *)(v0 + 44) = *(unsigned __int16 *)(39 * timer_decrement + saved_timer + 32)
                                     + (*(int *)(v0 + 129) >> 16)
                                     + 1;
              }
            }
            else
            {
              *(_DWORD *)(v0 + 60) = 0;
              ResetObjectVelocity();
              v0 = g_object_data_ptr;
              *(_DWORD *)(g_object_data_ptr + 350) &= ~0x10u;
              switch ( *(_DWORD *)(v0 + 346) )
              {
                case 0:
                  *(int *)((char *)object_data_ptr + 57221) = 0;
                  *(int *)((char *)object_data_ptr + 57089) = 0;
                  *(_DWORD *)(v0 + 48) = -1;
                  *(int *)((char *)object_data_ptr + 57327) = 0;
                  character_state_updater();
                  v0 = g_object_data_ptr;
                  break;
                case 1:
                  saved_attack_state_high = *(_DWORD *)(v0 + 52);
                  if ( saved_attack_state_high == *((unsigned __int16 *)object_data_ptr + 15058) )
                  {
                    if ( !*(_DWORD *)(v0 + 16) )
                    {
                      ClearObjectReferences();
                      return;
                    }
LABEL_156:
                    saved_attack_state_low = object_data_ptr[68];
                    *(_DWORD *)(v0 + 48) = saved_attack_state_high;
                    *(_DWORD *)(v0 + 44) = *(unsigned __int16 *)(39 * saved_attack_state_high
                                                               + saved_attack_state_low
                                                               + 32);
                  }
                  else
                  {
                    if ( saved_attack_state_high != *((unsigned __int16 *)object_data_ptr + 15059)
                      || !*(_DWORD *)(v0 + 16) )
                    {
                      goto LABEL_450;
                    }
                    *(_DWORD *)(v0 + 48) = saved_attack_state_high;
                    *(_DWORD *)(v0 + 44) = *(unsigned __int16 *)(39 * saved_attack_state_high + object_data_ptr[68] + 32);
                  }
                  break;
                case 2:
                  if ( (*(_BYTE *)(39 * *(_DWORD *)(v0 + 52) + object_data_ptr[68] + 35) & 0x20) != 0 )
                    goto LABEL_450;
                  goto LABEL_154;
                case 3:
                case 4:
                case 5:
LABEL_154:
                  if ( !*(_DWORD *)(v0 + 16) )
                    goto LABEL_450;
                  saved_attack_state_high = *(_DWORD *)(v0 + 52);
                  goto LABEL_156;
                default:
                  goto LABEL_450;
              }
            }
          }
        }
        reference_ptr = object_data_ptr[69];
        reference_count = reference_ptr + 16 * *(_DWORD *)(v0 + 44);
        switch ( *(_BYTE *)reference_count )
        {
          case 1:
            delay_frames = 0;
            delay_result = 1;
            if ( *(_DWORD *)(v0 + 92) )
              delay_result = -1;
            velocity_absolute_flag = *(_BYTE *)(reference_count + 9);
            if ( (velocity_absolute_flag & 1) != 0 )
              delay_frames = 1;
            velocity_direction = delay_result * g_char_data_array_end * *(__int16 *)(reference_count + 3);
            if ( (velocity_absolute_flag & 2) == 0 )
            {
              if ( delay_frames )
                *(_DWORD *)(v0 + 24) += velocity_direction;
              else
                *(_DWORD *)(v0 + 24) = velocity_direction;
            }
            command_flags = g_char_data_array_end * *(__int16 *)(reference_count + 5);
            if ( (velocity_absolute_flag & 4) == 0 )
            {
              if ( delay_frames )
                *(_DWORD *)(v0 + 28) += command_flags;
              else
                *(_DWORD *)(v0 + 28) = command_flags;
            }
            velocity_x_calc = delay_result * g_char_velocity_multiplier * *(__int16 *)(reference_count + 1);
            if ( (velocity_absolute_flag & 8) == 0 )
            {
              if ( delay_frames )
                *(_DWORD *)(v0 + 32) += velocity_x_calc;
              else
                *(_DWORD *)(v0 + 32) = velocity_x_calc;
            }
            velocity_y_calc = g_char_velocity_multiplier * *(__int16 *)(reference_count + 7);
            if ( (velocity_absolute_flag & 0x10) != 0 )
              goto LABEL_333;
            if ( delay_frames )
              *(_DWORD *)(v0 + 36) += velocity_y_calc;
            else
              *(_DWORD *)(v0 + 36) = velocity_y_calc;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 2:
            if ( !*(_BYTE *)(reference_count + 1) )
              goto LABEL_333;
            velocity_x_value = *(unsigned __int16 *)(reference_count + 2)
                             + (*(unsigned __int8 *)(reference_count + 4) << 16);
            switch ( *(_BYTE *)(reference_count + 1) )
            {
              case 1:
                *(_DWORD *)(v0 + 100) = velocity_x_value;
                ++*(_DWORD *)(v0 + 44);
                break;
              case 2:
                *(_DWORD *)(v0 + 108) = velocity_x_value;
                ++*(_DWORD *)(v0 + 44);
                break;
              case 3:
                *(_DWORD *)(v0 + 104) = velocity_x_value;
                ++*(_DWORD *)(v0 + 44);
                break;
              case 4:
                *(_DWORD *)(v0 + 112) = velocity_x_value;
                ++*(_DWORD *)(v0 + 44);
                break;
              case 5:
                *(_DWORD *)(v0 + 116) = velocity_x_value;
                ++*(_DWORD *)(v0 + 44);
                break;
              case 6:
                *(_DWORD *)(v0 + 120) = velocity_x_value;
                ++*(_DWORD *)(v0 + 44);
                break;
              default:
                goto LABEL_333;
            }
            goto LABEL_133;
          case 3:
            volume_control_function(object_data_ptr[2183] + 42 * *(unsigned __int16 *)(reference_count + 2));
            v0 = g_object_data_ptr;
            ++*(_DWORD *)(g_object_data_ptr + 44);
            goto LABEL_133;
          case 4:
            if ( *(int *)(v0 + 346) >= 2 )
              goto LABEL_209;
            if ( (*(_BYTE *)(reference_count + 1) & 4) != 0 )
              goto LABEL_209;
            animation_frame_count_temp = *(unsigned __int8 *)(reference_count + 12);
            attack_state_id_temp2 = *(_DWORD **)((char *)&object_data_ptr[animation_frame_count_temp + 14319] + 3);
            object_slot_index = (int *)((char *)&object_data_ptr[animation_frame_count_temp + 14319] + 3);
            if ( !attack_state_id_temp2 )
              goto LABEL_209;
            object_pool_ptr = 0;
            object_slot_ptr = &g_object_pool;
            while ( object_slot_ptr != attack_state_id_temp2 )
            {
              ++object_pool_ptr;
              object_slot_ptr = (_DWORD *)((char *)object_slot_ptr + 382);
              if ( object_pool_ptr >= 1024 )
                goto LABEL_209;
            }
            if ( *(_WORD *)(reference_count + 5) )
            {
              object_index = object_data_ptr[68];
              object_pool_iterator = *(unsigned __int16 *)(reference_count + 5);
              *(_DWORD *)(v0 + 48) = object_pool_iterator;
              LOWORD(object_pool_iterator) = *(_WORD *)(39 * object_pool_iterator + object_index + 32);
              sprite_data_base_ptr2 = *(unsigned __int8 *)(reference_count + 7);
              *(_DWORD *)(v0 + 44) = (unsigned __int16)object_pool_iterator + sprite_data_base_ptr2 - 1;
              *(_DWORD *)(v0 + 44) = (unsigned __int16)object_pool_iterator + sprite_data_base_ptr2;
              goto LABEL_133;
            }
            *object_slot_ptr = 1;
            *object_slot_index = 0;
LABEL_209:
            if ( !*(_WORD *)(reference_count + 2) )
              goto LABEL_333;
            variable_value_result2 = *(_BYTE *)(reference_count + 1);
            damage_value = variable_value_result2 & 0x40;
            switch ( *(_DWORD *)(v0 + 346) )
            {
              case 2:
                if ( (*(_BYTE *)(39 * *(unsigned __int16 *)(reference_count + 2) + object_data_ptr[68] + 35) & 9) == 0 )
                {
LABEL_216:
                  damage_value = 1;
LABEL_217:
                  attack_state_id_ptr = *(__int16 *)(reference_count + 8) << 16;
                  frame_offset_ptr = *(__int16 *)(reference_count + 10) << 16;
                  goto LABEL_218;
                }
                damage_value = 0;
                break;
              case 3:
                goto LABEL_216;
              case 4:
                damage_value = 0;
                break;
              default:
                if ( (*(_BYTE *)(reference_count + 1) & 0x40) != 0 )
                  goto LABEL_217;
                break;
            }
            spawn_y_pos = *(__int16 *)(reference_count + 8);
            if ( (*(_BYTE *)(v0 + 92) & 1) != 0 )
              attack_state_id_ptr = *(_DWORD *)(v0 + 8) - (spawn_y_pos << 16);
            else
              attack_state_id_ptr = *(_DWORD *)(v0 + 8) + (spawn_y_pos << 16);
            frame_offset_ptr = *(_DWORD *)(v0 + 12) + (*(__int16 *)(reference_count + 10) << 16);
LABEL_218:
            spawn_x_pos = *(_DWORD *)(v0 + 4);
            if ( (variable_value_result2 & 3) != 0 )
            {
              if ( (variable_value_result2 & 3) == 1 )
              {
                if ( ++spawn_x_pos > 127 )
                  spawn_x_pos = 127;
              }
              else if ( (variable_value_result2 & 3) == 2 )
              {
                spawn_x_pos = *(unsigned __int8 *)(reference_count + 13);
              }
            }
            else if ( --spawn_x_pos < 10 )
            {
              spawn_x_pos = 10;
            }
            hit_priority_value = create_game_object(*(_DWORD *)v0, spawn_x_pos, attack_state_id_ptr, frame_offset_ptr);
            v0 = g_object_data_ptr;
            *(_DWORD *)((char *)hit_priority_value + 346) = 1;
            *(_DWORD *)((char *)hit_priority_value + 342) = *(_DWORD *)(v0 + 342);
            switch ( *(_DWORD *)(v0 + 346) )
            {
              case 0:
              case 1:
              case 5:
                *(_DWORD *)((char *)hit_priority_value + 346) = 1;
                break;
              case 2:
                *(_DWORD *)((char *)hit_priority_value + 346) = 2;
                break;
              case 3:
                *(_DWORD *)((char *)hit_priority_value + 346) = 3;
                break;
              case 4:
                *(_DWORD *)((char *)hit_priority_value + 346) = 4;
                break;
              default:
                break;
            }
            spawn_x_offset = *(_DWORD *)(v0 + 92);
            hit_priority_value[5] = *(_DWORD *)(v0 + 20);
            spawned_object_ptr = *(_WORD *)(reference_count + 2);
            hit_priority_value[23] = spawn_x_offset;
            hit_priority_value[12] = spawned_object_ptr;
            hit_priority_value[11] = *(unsigned __int8 *)(reference_count + 4)
                                   + *(unsigned __int16 *)(39 * spawned_object_ptr + object_data_ptr[68] + 32);
            if ( !damage_value )
              hit_priority_value[10] |= 0x40000000u;
            if ( *(int *)(v0 + 346) < 2 )
            {
              facing_direction = *(_BYTE *)(reference_count + 1);
              if ( (facing_direction & 4) == 0 )
                *(int *)((char *)&object_data_ptr[*(unsigned __int8 *)(reference_count + 12) + 14319] + 3) = (int)hit_priority_value;
              if ( (facing_direction & 8) != 0 )
                hit_priority_value[10] |= 0x80000000;
            }
            if ( (*(_BYTE *)(reference_count + 1) & 0x20) == 0 )
              goto LABEL_333;
            sprite_id = hit_priority_value[10] | 0x20000000;
            *(_WORD *)((char *)hit_priority_value + 303) = *(_WORD *)(reference_count + 10);
            hit_priority_value[10] = sprite_id;
            *(_WORD *)((char *)hit_priority_value + 301) = *(_WORD *)(reference_count + 8);
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 5:
          case 0x29:
            if ( !*(_DWORD *)(v0 + 346) )
              goto LABEL_139;
            goto LABEL_450;
          case 7:
            if ( *(_DWORD *)(v0 + 346) )
              goto LABEL_333;
            if ( !*(_WORD *)(reference_count + 2) )
              goto LABEL_333;
            input_history_value = *(_DWORD **)((char *)object_data_ptr + 57081);
            if ( !input_history_value )
              goto LABEL_333;
            target_attack_state = *(_BYTE *)(reference_count + 1);
            hitbox_object_ptr = g_hit_priority_table[*(_DWORD *)(v0 + 20) & 1];
            hitbox_flags = input_history_value[5] & 1;
            if ( (target_attack_state & 1) != 0 )
            {
              *(_DWORD *)(v0 + 4) = hitbox_object_ptr + 1;
              hit_priority_self = g_hit_priority_table[hitbox_flags] - 1;
            }
            else
            {
              *(_DWORD *)(v0 + 4) = hitbox_object_ptr - 1;
              hit_priority_self = g_hit_priority_table[hitbox_flags] + 1;
            }
            hit_priority_other = *(_BYTE *)(v0 + 92);
            input_history_value[1] = hit_priority_self;
            initialization_flag = (hit_priority_other & 1) == 0;
            hit_priority_target = *(__int16 *)(reference_count + 4);
            if ( initialization_flag )
            {
              input_history_value[2] = *(_DWORD *)(v0 + 8) + (hit_priority_target << 16);
              if ( (target_attack_state & 4) != 0 )
              {
                input_history_value[23] = 0;
                goto LABEL_296;
              }
            }
            else
            {
              input_history_value[2] = *(_DWORD *)(v0 + 8) - (hit_priority_target << 16);
              if ( (target_attack_state & 4) == 0 )
              {
                input_history_value[23] = 0;
                goto LABEL_296;
              }
            }
            input_history_value[23] = 1;
LABEL_296:
            input_history_value[3] = *(_DWORD *)(v0 + 12) + (*(__int16 *)(reference_count + 6) << 16);
            ClearObjectHitboxData(v0);
            facing_direction_temp = *(_DWORD *)((char *)input_history_value + 346);
            input_history_value[6] = 0;
            input_history_value[7] = 0;
            input_history_value[8] = 0;
            input_history_value[9] = 0;
            if ( !facing_direction_temp )
            {
              hitbox_x_offset = *(_WORD *)(reference_count + 2);
              hitbox_object_state = 57407 * *(_DWORD *)((char *)input_history_value + 342);
              hitbox_sprite_id = *(int *)((char *)&g_hitbox_sprite_data_ptrs + hitbox_object_state);
              input_history_value[4] = -1;
              *(_WORD *)(hitbox_sprite_id + 3) = ((*(_BYTE *)(reference_count + 1) & 0xC) << 12)
                                               | *(__int16 *)((char *)&g_hitbox_sprite_flags[3 * hitbox_x_offset]
                                                            + hitbox_object_state)
                                               & 0x1FFF;
              hitbox_character_offset = *(unsigned __int16 *)(reference_count + 2);
              *(_BYTE *)hitbox_sprite_id = 12;
              input_history_value[12] = 0;
              input_history_value[11] = 1;
              hitbox_sprite_data_ptr = hitbox_object_state + 6 * hitbox_character_offset;
              hitbox_sprite_id_temp = *(__int16 *)((char *)&g_hitbox_hitstun_values + hitbox_sprite_data_ptr);
              *(_WORD *)(hitbox_sprite_id + 5) = *(__int16 *)((char *)&g_hitbox_damage_values + hitbox_sprite_data_ptr);
              *(_WORD *)(hitbox_sprite_id + 7) = hitbox_sprite_id_temp;
              ClearObjectHitboxData((int)input_history_value);
              ClearObjectFlag297((int)input_history_value);
              hitbox_character_offset_temp = *(__int16 *)(reference_count + 4);
              hitbox_damage = *(__int16 *)(reference_count + 6);
              *(int *)((char *)object_data_ptr + 57327) = *(_BYTE *)(reference_count + 1) | 0x20;
              hitbox_x_offset_temp = *(_DWORD *)((char *)input_history_value + 350);
              LOBYTE(hitbox_x_offset_temp) = hitbox_x_offset_temp & 0xF0 | 0xA;
              *(int *)((char *)object_data_ptr + 57335) = hitbox_character_offset_temp << 16;
              *(int *)((char *)object_data_ptr + 57339) = hitbox_damage << 16;
              *(_DWORD *)((char *)input_history_value + 350) = hitbox_x_offset_temp;
            }
            v0 = g_object_data_ptr;
            input_history_value[16] = -1;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 9:
            if ( !*(_BYTE *)(reference_count + 1) )
              goto LABEL_333;
            animation_frame_count = *(_WORD *)(reference_count + 2);
            if ( !animation_frame_count )
              goto LABEL_333;
            frame_offset = object_data_ptr[68];
            saved_attack_state_temp = *(unsigned __int8 *)(reference_count + 4);
            *(_DWORD *)(v0 + 129) = *(_DWORD *)(v0 + 48)
                                  + ((*(_DWORD *)(v0 + 44)
                                    - *(unsigned __int16 *)(39 * *(_DWORD *)(v0 + 48) + frame_offset + 32)) << 16);
            *(_BYTE *)(v0 + 124) = *(_BYTE *)(reference_count + 1);
            *(_DWORD *)(v0 + 125) = animation_frame_count + (saved_attack_state_temp << 16);
            *(_DWORD *)(v0 + 48) = animation_frame_count;
            sprite_data_base_temp2 = *(unsigned __int16 *)(39 * animation_frame_count + frame_offset + 32)
                                   + saved_attack_state_temp
                                   - 1;
            *(_DWORD *)(v0 + 44) = sprite_data_base_temp2;
            *(_DWORD *)(v0 + 44) = sprite_data_base_temp2 + 1;
            goto LABEL_133;
          case 0xA:
            if ( !*(_WORD *)(reference_count + 1) )
              goto LABEL_333;
            velocity_y_value = object_data_ptr[68];
            jump_target_value = *(unsigned __int16 *)(reference_count + 1);
            *(_DWORD *)(v0 + 48) = jump_target_value;
            sprite_data_base_temp = 39 * jump_target_value;
            goto LABEL_193;
          case 0xB:
            if ( !*(_WORD *)(reference_count + 1) )
              goto LABEL_333;
            velocity_y_value = object_data_ptr[68];
            *(_DWORD *)(v0 + 133) = *(_DWORD *)(v0 + 48)
                                  + ((*(_DWORD *)(v0 + 44)
                                    - *(unsigned __int16 *)(39 * *(_DWORD *)(v0 + 48) + velocity_y_value + 32)) << 16);
            frame_offset_temp = *(unsigned __int16 *)(reference_count + 1);
            *(_DWORD *)(v0 + 48) = frame_offset_temp;
            sprite_data_base_temp = 39 * frame_offset_temp;
LABEL_193:
            attack_state_id_temp = *(_WORD *)(sprite_data_base_temp + velocity_y_value + 32);
            sprite_offset_calc = *(unsigned __int8 *)(reference_count + 3);
            *(_DWORD *)(v0 + 44) = attack_state_id_temp + sprite_offset_calc - 1;
            *(_DWORD *)(v0 + 44) = attack_state_id_temp + sprite_offset_calc;
            goto LABEL_133;
          case 0xC:
            if ( !*(_DWORD *)(v0 + 346) )
              *(int *)((char *)object_data_ptr + 57359) = 1;
            previous_attack_state = *(_WORD *)(reference_count + 1);
            sprite_data_base_ptr = -1;
            *(_DWORD *)(v0 + 16) = -1;
            if ( previous_attack_state )
              sprite_data_base_ptr = g_delay_frame_multiplier * previous_attack_state + *(_DWORD *)(v0 + 60);
            *(_DWORD *)(v0 + 60) = sprite_data_base_ptr;
            character_data_iterator2 = 0;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 0xE:
            hitbox_object_ptr4 = *(_BYTE *)(reference_count + 1);
            if ( hitbox_object_ptr4 )
            {
              if ( (*(_BYTE *)(reference_count + 8) & 1) != 0 )
              {
                target_character_data_ptr = (_DWORD *)((char *)&g_effect_data_array + 57407 * *(_DWORD *)(v0 + 342));
                *target_character_data_ptr = hitbox_object_ptr4;
                target_character_data_ptr[1] = *(char *)(reference_count + 2);
                target_character_data_ptr[2] = *(char *)(reference_count + 3);
                target_character_data_ptr[3] = *(char *)(reference_count + 4);
                target_character_data_ptr[4] = *(char *)(reference_count + 5);
                target_character_data_ptr[6] = *(_DWORD *)(v0 + 68);
                target_character_data_ptr[7] = *(_DWORD *)(v0 + 72);
                target_character_data_ptr[8] = *(_DWORD *)(v0 + 76);
                target_character_data_ptr[9] = *(_DWORD *)(v0 + 80);
                effect_id = *(unsigned __int16 *)(reference_count + 6);
                target_character_data_ptr[10] = effect_id;
                target_character_data_ptr[5] = effect_id;
              }
              if ( (*(_BYTE *)(reference_count + 8) & 2) != 0 )
              {
                effect_data_ptr = *(int *)((char *)object_data_ptr + 57081);
                if ( effect_data_ptr )
                {
                  effect_param1 = (_DWORD *)((char *)&g_effect_data_array + 57407 * *(_DWORD *)(effect_data_ptr + 342));
                  *effect_param1 = hitbox_object_ptr4;
                  effect_param1[1] = *(char *)(reference_count + 2);
                  effect_param1[2] = *(char *)(reference_count + 3);
                  effect_param1[3] = *(char *)(reference_count + 4);
                  effect_param1[4] = *(char *)(reference_count + 5);
                  effect_param1[6] = 0;
                  effect_param1[7] = 0;
                  effect_param1[8] = 0;
                  effect_param1[9] = 0;
                  hitbox_object_ptr5 = *(unsigned __int16 *)(reference_count + 6);
                  effect_param1[10] = hitbox_object_ptr5;
                  effect_param1[5] = hitbox_object_ptr5;
                }
              }
              effect_data_ptr2 = *(_BYTE *)(reference_count + 8);
              if ( (effect_data_ptr2 & 4) != 0 )
              {
                g_effect_timer_countdown2 = 0;
                g_effect_id_1 = hitbox_object_ptr4;
                g_effect_param1_1 = 0;
                g_effect_param2_1 = *(char *)(reference_count + 2);
                g_effect_param3_1 = 0;
                g_effect_param4_1 = *(char *)(reference_count + 3);
                g_effect_param5_1 = 0;
                g_effect_param6_1 = *(char *)(reference_count + 4);
                g_effect_param7_1 = *(char *)(reference_count + 5);
                g_effect_timer_1 = *(unsigned __int16 *)(reference_count + 6);
                g_timer_countdown2 = g_effect_timer_1;
              }
              if ( (effect_data_ptr2 & 8) != 0 )
              {
                effect_param6 = *(char *)(reference_count + 4);
                g_effect_param2_2 = *(char *)(reference_count + 2);
                effect_flags = *(char *)(reference_count + 5);
                g_effect_id_2 = hitbox_object_ptr4;
                g_effect_param4_2 = effect_param6;
                g_effect_param3_2 = *(char *)(reference_count + 3);
                shake_effect_param1 = *(unsigned __int16 *)(reference_count + 6);
                g_effect_param5_2 = effect_flags;
                g_effect_param6_2 = 0;
                g_effect_param7_2 = 0;
                g_effect_param8_2 = 0;
                g_effect_param9_2 = 0;
                g_effect_timer_2 = shake_effect_param1;
                g_timer_countdown1 = shake_effect_param1;
              }
            }
            if ( *(_BYTE *)(reference_count + 9) )
            {
              shake_effect_param2 = *(unsigned __int8 *)(reference_count + 10);
              g_shake_effect_1 = *(unsigned __int8 *)(reference_count + 9);
              g_shake_effect_param1_1 = 0;
              shake_effect_param3 = *(unsigned __int8 *)(reference_count + 11);
              g_shake_effect_param2_1 = shake_effect_param2;
              g_shake_effect_param3_1 = shake_effect_param3;
              g_shake_effect_param4_1 = shake_effect_param3;
            }
            shake_effect_param4 = *(_BYTE *)(reference_count + 12);
            if ( !shake_effect_param4 )
              goto LABEL_333;
            g_shake_effect_param1_2 = 0;
            g_shake_effect_2 = shake_effect_param4;
            g_shake_effect_param2_2 = *(unsigned __int8 *)(reference_count + 13);
            g_shake_effect_param3_2 = *(unsigned __int8 *)(reference_count + 14);
            g_shake_effect_param4_2 = g_shake_effect_param3_2;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 0x10:
            if ( (*(_BYTE *)(reference_count + 5) & 1) != 0 )
            {
              if ( *(int *)((char *)&g_char_value_current + 57407 * *(_DWORD *)(v0 + 342)) <= *(unsigned __int8 *)(reference_count + 6) )
              {
                char_value_current = *(_WORD *)(reference_count + 2);
                *(_DWORD *)(v0 + 48) = char_value_current;
                if ( !char_value_current )
                {
LABEL_356:
                  LOWORD(char_value_max) = ai_behavior_processor(*(int *)((char *)&g_char_ai_behavior_state
                                                                        + 57407 * *(_DWORD *)(v0 + 342)));
                  v0 = g_object_data_ptr;
                  if ( char_value_max )
                    *(_DWORD *)(g_object_data_ptr + 48) = char_value_max;
                }
LABEL_358:
                player_id_temp = *(_DWORD *)(v0 + 48);
                goto LABEL_359;
              }
              shake_effect_param5 = 57407 * *(_DWORD *)(v0 + 342);
              shake_effect_id2 = ReturnCharValue(*(_BYTE *)(reference_count + 7));
              character_offset_temp = *(int *)((char *)&g_char_value_current + shake_effect_param5);
              *(int *)((char *)&g_char_value_current + shake_effect_param5) = shake_effect_id2 + character_offset_temp;
              if ( shake_effect_id2 + character_offset_temp < 0 )
                *(int *)((char *)&g_char_value_current + shake_effect_param5) = 0;
              char_value_result = *(int *)((char *)&g_char_value_max + shake_effect_param5);
              if ( *(int *)((char *)&g_char_value_current + shake_effect_param5) >= char_value_result )
              {
                *(int *)((char *)&g_char_value_current + shake_effect_param5) = char_value_result;
                *(int *)((char *)&g_char_value_flag + shake_effect_param5) = 0;
                v0 = g_object_data_ptr;
                ++*(_DWORD *)(g_object_data_ptr + 44);
                goto LABEL_133;
              }
LABEL_332:
              v0 = g_object_data_ptr;
            }
            else if ( *(int *)((char *)&g_char_value_current + 57407 * *(_DWORD *)(v0 + 342)) >= *(unsigned __int8 *)(reference_count + 6)
                   && *(_WORD *)(reference_count + 2) )
            {
              animation_frame_count_result2 = *(unsigned __int16 *)(reference_count + 2);
              *(_DWORD *)(v0 + 48) = animation_frame_count_result2;
              if ( !animation_frame_count_result2 )
                goto LABEL_356;
              goto LABEL_358;
            }
LABEL_333:
            ++*(_DWORD *)(v0 + 44);
LABEL_133:
            if ( !character_data_iterator2 )
              return;
            position_y_temp = character_data_iterator2;
            break;
          case 0x11:
            if ( (*(_BYTE *)(reference_count + 5) & 1) != 0 )
            {
              if ( *(int *)((char *)object_data_ptr + 57093) > *(unsigned __int16 *)(reference_count + 6) )
                goto LABEL_333;
            }
            else if ( *(int *)((char *)object_data_ptr + 57093) < *(unsigned __int16 *)(reference_count + 6)
                   || !*(_WORD *)(reference_count + 2) )
            {
              goto LABEL_333;
            }
            hitbox_x_offset2 = object_data_ptr[68];
            hitbox_state_flags2 = *(_WORD *)(reference_count + 2);
            *(_DWORD *)(v0 + 48) = hitbox_state_flags2;
            *(_DWORD *)(v0 + 44) = *(unsigned __int16 *)(39 * hitbox_state_flags2 + hitbox_x_offset2 + 32)
                                 + *(unsigned __int8 *)(reference_count + 4)
                                 - 1;
            if ( *(_DWORD *)(v0 + 48) )
              goto LABEL_333;
            LOWORD(sprite_data_base_ptr3) = ai_behavior_processor(*(int *)((char *)object_data_ptr + 57173));
            if ( sprite_data_base_ptr3 )
            {
              v0 = g_object_data_ptr;
              target_attack_state_id = object_data_ptr[68];
              *(_DWORD *)(g_object_data_ptr + 48) = sprite_data_base_ptr3;
              ai_result = *(unsigned __int16 *)(39 * sprite_data_base_ptr3 + target_attack_state_id + 32) - 1;
              *(_DWORD *)(v0 + 44) = ai_result;
              *(_DWORD *)(v0 + 44) = ai_result + 1;
              goto LABEL_133;
            }
            goto LABEL_332;
          case 0x14:
            if ( *(_DWORD *)(v0 + 346) )
              goto LABEL_333;
            hitbox_y_offset = *(_DWORD **)((char *)object_data_ptr + 57081);
            if ( !hitbox_y_offset )
              goto LABEL_333;
            hitbox_state_flags = *(_BYTE *)(reference_count + 1);
            hitbox_object_ptr2 = g_hit_priority_table[*(_DWORD *)(v0 + 20) & 1];
            hitbox_flags2 = hitbox_y_offset[5] & 1;
            if ( (hitbox_state_flags & 1) != 0 )
            {
              *(_DWORD *)(v0 + 4) = hitbox_object_ptr2 + 1;
              hit_priority_self2 = g_hit_priority_table[hitbox_flags2] - 1;
            }
            else
            {
              *(_DWORD *)(v0 + 4) = hitbox_object_ptr2 - 1;
              hit_priority_self2 = g_hit_priority_table[hitbox_flags2] + 1;
            }
            hit_priority_other2 = *(_BYTE *)(v0 + 92);
            hitbox_y_offset[1] = hit_priority_self2;
            initialization_flag = (hit_priority_other2 & 1) == 0;
            hit_priority_target2 = *(__int16 *)(reference_count + 4);
            if ( initialization_flag )
            {
              hitbox_y_offset[2] = *(_DWORD *)(v0 + 8) + (hit_priority_target2 << 16);
              if ( (hitbox_state_flags & 4) != 0 )
              {
                hitbox_y_offset[23] = 0;
                goto LABEL_310;
              }
            }
            else
            {
              hitbox_y_offset[2] = *(_DWORD *)(v0 + 8) - (hit_priority_target2 << 16);
              if ( (hitbox_state_flags & 4) == 0 )
              {
                hitbox_y_offset[23] = 0;
                goto LABEL_310;
              }
            }
            hitbox_y_offset[23] = 1;
LABEL_310:
            hitbox_y_offset[3] = *(_DWORD *)(v0 + 12) + (*(__int16 *)(reference_count + 6) << 16);
            ClearObjectHitboxData(v0);
            if ( *(_DWORD *)((char *)hitbox_y_offset + 346) )
              goto LABEL_332;
            if ( *(_BYTE *)(reference_count + 2) )
              hitbox_y_offset[14] = *(unsigned __int16 *)((char *)&g_hitbox_sprite_ids
                                                        + 57407 * *(_DWORD *)((char *)hitbox_y_offset + 342)
                                                        + 4 * *(unsigned __int8 *)(reference_count + 2));
            facing_direction_temp2 = *(_DWORD *)((char *)hitbox_y_offset + 350);
            v0 = g_object_data_ptr;
            hitbox_y_offset[16] = 0;
            LOBYTE(facing_direction_temp2) = facing_direction_temp2 & 0xF0 | 0xA;
            *(int *)((char *)object_data_ptr + 57327) = 0;
            *(_DWORD *)((char *)hitbox_y_offset + 350) = facing_direction_temp2;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 0x15:
            sprite_data_base_ptr4 = (char *)&g_character_data_base + 57407 * *(_DWORD *)(v0 + 342);
            selected_action_ptr = *(__int16 *)(reference_count + 6);
            if ( *(_WORD *)(reference_count + 2) )
            {
              health_damage_manager((int)sprite_data_base_ptr4, *(__int16 *)(reference_count + 2));
              v0 = g_object_data_ptr;
            }
            UpdateAnimationFrame(*(_DWORD *)(v0 + 342), *(__int16 *)(reference_count + 4));
            animation_frame_count_result = *(_DWORD *)(sprite_data_base_ptr4 + 57081);
            if ( animation_frame_count_result )
            {
              character_data_ptr2 = *(_DWORD *)(animation_frame_count_result + 342);
            }
            else
            {
              hitbox_object_ptr3 = *(_DWORD *)(sprite_data_base_ptr4 + 57193);
              if ( !hitbox_object_ptr3 )
                goto LABEL_332;
              character_data_ptr2 = *(_DWORD *)(hitbox_object_ptr3 + 342);
            }
            hitbox_player_id = (char *)&g_character_data_base + 57407 * character_data_ptr2;
            if ( hitbox_player_id )
            {
              if ( selected_action_ptr )
                health_damage_manager((int)hitbox_player_id, selected_action_ptr);
              UpdateAnimationFrame(
                *(_DWORD *)(*(_DWORD *)(hitbox_player_id + 57077) + 342),
                *(__int16 *)(reference_count + 8));
            }
            goto LABEL_332;
          case 0x16:
            reference_flags = *(_BYTE *)(reference_count + 1) & 1;
            if ( (*(_BYTE *)(reference_count + 1) & 2) != 0 )
            {
LABEL_281:
              if ( !reference_flags )
                goto LABEL_333;
            }
            else
            {
              state_flags_temp = *(_DWORD *)(v0 + 342);
              input_check_flag = g_p1_input_history[1024 * state_flags_temp + g_input_buffer_index];
              switch ( *(_BYTE *)(reference_count + 7) )
              {
                case 1:
                  if ( *(_DWORD *)(v0 + 12) < *(_DWORD *)(v0 + 88) )
                    goto LABEL_280;
                  goto LABEL_278;
                case 2:
                  if ( *(_DWORD *)(v0 + 12) >= *(_DWORD *)(v0 + 88) && (input_check_flag & 8) == 0 )
                    goto LABEL_278;
                  goto LABEL_280;
                case 3:
                  if ( *(_DWORD *)(v0 + 12) >= *(_DWORD *)(v0 + 88) )
                    goto LABEL_265;
                  goto LABEL_280;
                case 4:
                  if ( (g_char_state_flags[57407 * state_flags_temp] & 8) != 0 && *(_DWORD *)(v0 + 92) )
                    goto LABEL_269;
                  goto LABEL_273;
                case 5:
                  if ( (g_char_state_flags[57407 * state_flags_temp] & 8) != 0 && *(_DWORD *)(v0 + 92) )
                  {
LABEL_273:
                    if ( (input_check_flag & 2) == 0 )
                    {
LABEL_280:
                      reference_flags = *(_BYTE *)(reference_count + 1) & 1;
                      goto LABEL_281;
                    }
                  }
                  else
                  {
LABEL_269:
                    if ( (input_check_flag & 1) == 0 )
                      goto LABEL_280;
                  }
LABEL_278:
                  if ( (*(_BYTE *)(reference_count + 1) & 1) != 0 )
                    goto LABEL_333;
                  break;
                case 6:
                  if ( (input_check_flag & 4) != 0 )
                    goto LABEL_278;
                  goto LABEL_280;
                case 7:
LABEL_265:
                  if ( (input_check_flag & 8) != 0 )
                    goto LABEL_278;
                  goto LABEL_280;
                case 8:
                  if ( (input_check_flag & 0xF) == 0 )
                    goto LABEL_278;
                  goto LABEL_280;
                default:
                  goto LABEL_280;
              }
            }
            if ( !*(_WORD *)(reference_count + 2) )
              goto LABEL_333;
            player_id_temp = *(unsigned __int16 *)(reference_count + 2);
            *(_DWORD *)(v0 + 48) = player_id_temp;
LABEL_359:
            target_attack_state_id2 = *(_WORD *)(39 * player_id_temp + object_data_ptr[68] + 32);
            ai_result2 = *(unsigned __int8 *)(reference_count + 4);
            *(_DWORD *)(v0 + 44) = target_attack_state_id2 + ai_result2 - 1;
            *(_DWORD *)(v0 + 44) = target_attack_state_id2 + ai_result2;
            goto LABEL_133;
          case 0x17:
            *(_DWORD *)(v0 + 297) = reference_count;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 0x18:
            initialization_flag = *(_WORD *)(reference_count + 5) == 0;
            spawn_flags = (int *)(v0 + 4 * *(unsigned __int8 *)(reference_count + 9) + 137);
            *spawn_flags = reference_count;
            if ( initialization_flag || !*(_WORD *)(reference_count + 7) )
              goto LABEL_255;
            if ( *(_DWORD *)(v0 + 346) )
              goto LABEL_333;
            object_flags = *(unsigned __int8 *)(reference_count + 10);
            *(int *)((char *)object_data_ptr + 57221) = (object_flags & 1) != 0;
            if ( (object_flags & 2) == 0 )
              goto LABEL_333;
            reference_slot_ptr = *(_DWORD *)(v0 + 350);
            LOBYTE(reference_slot_ptr) = reference_slot_ptr & 0xEF;
            *(_DWORD *)(v0 + 350) = reference_slot_ptr;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 0x19:
            initialization_flag = *(_WORD *)(reference_count + 5) == 0;
            spawn_flags = (int *)(v0 + 4 * *(unsigned __int8 *)(reference_count + 9) + 217);
            *spawn_flags = reference_count;
            if ( !initialization_flag && *(_WORD *)(reference_count + 7) )
              goto LABEL_333;
LABEL_255:
            *spawn_flags = 0;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 0x1A:
            if ( !*(_DWORD *)(v0 + 346) && *(_BYTE *)(reference_count + 1) )
            {
              sprite_offset_calc2 = *(unsigned __int8 *)(reference_count + 1);
              *(_DWORD *)(v0 + 64) += sprite_offset_calc2;
              target_attack_state_id3 = *(_DWORD *)(v0 + 342);
              shake_effect_param6 = 57407 * target_attack_state_id3;
              *(int *)((char *)&g_input_override_value + shake_effect_param6) = g_p1_input_history[1024
                                                                                                 * target_attack_state_id3
                                                                                                 + g_input_buffer_index];
              *(int *)((char *)&g_input_override_active + shake_effect_param6) = 1;
              player_id_temp2 = &g_object_delay_timers;
              character_offset_temp2 = 1024;
              do
              {
                if ( *(player_id_temp2 - 16) == 4
                  && *(_DWORD *)((char *)player_id_temp2 + 278) == target_attack_state_id3
                  && (*((_BYTE *)player_id_temp2 - 21) & 0x20) != 0 )
                {
                  *player_id_temp2 += sprite_offset_calc2;
                }
                player_id_temp2 = (_DWORD *)((char *)player_id_temp2 + 382);
                --character_offset_temp2;
              }
              while ( character_offset_temp2 );
            }
            object_pool_iterator2 = *(_BYTE *)(reference_count + 2);
            loop_continue_flag = object_pool_iterator2;
            if ( !object_pool_iterator2 )
              goto LABEL_333;
            object_pool_loop_count = 0;
            object_index2 = (int)&g_character_data_array;
            do
            {
              if ( object_pool_loop_count != *(_DWORD *)(v0 + 342) && *(_DWORD *)(object_index2 - 48317) )
              {
                delay_frames2 = *(_DWORD *)object_index2;
                spawn_flag_value = 1024;
                *(_DWORD *)(delay_frames2 + 64) += object_pool_iterator2;
                *(_DWORD *)(object_index2 + 270) = g_p1_input_history[1024 * *(_DWORD *)(delay_frames2 + 342)
                                                                    + g_input_buffer_index];
                *(_DWORD *)(object_index2 + 266) = 1;
                character_data_iterator = &g_object_delay_timers;
                do
                {
                  if ( *(character_data_iterator - 16) == 4
                    && *(_DWORD *)((char *)character_data_iterator + 278) == *(_DWORD *)(*(_DWORD *)object_index2 + 342)
                    && (*(character_data_iterator - 6) & 0x20000000) != 0 )
                  {
                    *character_data_iterator += object_pool_iterator2;
                  }
                  character_data_iterator = (_DWORD *)((char *)character_data_iterator + 382);
                  --spawn_flag_value;
                }
                while ( spawn_flag_value );
                object_pool_iterator2 = loop_continue_flag;
              }
              object_index2 += 57407;
              ++object_pool_loop_count;
            }
            while ( object_index2 < 5570157 );
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 0x1E:
            object_ptr_temp = *(_WORD *)(reference_count + 4);
            *(int *)((char *)object_data_ptr + 57233) = *(_DWORD *)reference_count;
            *(_WORD *)((char *)object_data_ptr + 57237) = object_ptr_temp;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 0x1F:
            object_ptr_temp2 = *(unsigned __int8 *)(reference_count + 4);
            if ( object_ptr_temp2 >> 6 )
            {
              if ( object_ptr_temp2 >> 6 == 1 )
              {
                variable_id = (__int16 *)((char *)object_data_ptr + 2 * (object_ptr_temp2 & 0x3F) + 57239);
                action_conflict_check = (int)variable_id;
              }
              else if ( object_ptr_temp2 >> 6 == 2 )
              {
                variable_id = (__int16 *)(2 * (object_ptr_temp2 & 0x3F) + 4478640);
                action_conflict_check = (int)variable_id;
              }
              else
              {
                variable_id = (__int16 *)action_conflict_check;
              }
            }
            else
            {
              variable_id = (__int16 *)(v0 + 2 * (object_ptr_temp2 & 0x3F) + 305);
              action_conflict_check = (int)variable_id;
            }
            if ( *(char *)(reference_count + 5) >= 0 )
            {
              LOWORD(variable_ptr) = *(_WORD *)(reference_count + 7);
              LOWORD(sprite_data_array_ptr) = variable_ptr;
            }
            else
            {
              variable_value = *(unsigned __int8 *)(reference_count + 6);
              switch ( variable_value >> 6 )
              {
                case 0u:
                  LOWORD(variable_ptr) = *(_WORD *)(v0 + 2 * (variable_value & 0x3F) + 305);
                  LOWORD(sprite_data_array_ptr) = variable_ptr;
                  break;
                case 1u:
                  LOWORD(variable_ptr) = *(_WORD *)((char *)object_data_ptr + 2 * (variable_value & 0x3F) + 57239);
                  LOWORD(sprite_data_array_ptr) = variable_ptr;
                  break;
                case 2u:
                  LOWORD(variable_ptr) = g_global_variable_array_FM2K_SYSTEM_VARS[variable_value & 0x3F];
                  LOWORD(sprite_data_array_ptr) = variable_ptr;
                  break;
                case 3u:
                  switch ( variable_value & 0x3F )
                  {
                    case 0u:
                      source_variable_id = *(_DWORD *)(v0 + 8);
                      goto LABEL_400;
                    case 1u:
                      source_variable_id = *(_DWORD *)(v0 + 12);
                      goto LABEL_400;
                    case 2u:
                      LOWORD(variable_ptr) = g_screen_x;
                      LOWORD(sprite_data_array_ptr) = g_screen_x;
                      goto LABEL_410;
                    case 3u:
                      LOWORD(variable_ptr) = g_screen_y;
                      LOWORD(sprite_data_array_ptr) = g_screen_y;
                      goto LABEL_410;
                    case 4u:
                      source_variable_id = *(_DWORD *)(*(_DWORD *)(v0 + 378) + 8);
                      goto LABEL_400;
                    case 5u:
                      source_variable_id = *(_DWORD *)(*(_DWORD *)(v0 + 378) + 12);
LABEL_400:
                      variable_ptr = source_variable_id / 0x10000;
                      sprite_data_array_ptr = source_variable_id / 0x10000;
                      break;
                    case 6u:
                      variable_ptr = g_score_value / 100;
                      sprite_data_array_ptr = g_score_value / 100;
                      break;
                    case 7u:
                      LOWORD(variable_ptr) = g_game_timer;
                      LOWORD(sprite_data_array_ptr) = g_game_timer;
                      break;
                    default:
                      goto LABEL_409;
                  }
                  break;
                default:
LABEL_409:
                  LOWORD(variable_ptr) = sprite_data_array_ptr;
                  break;
              }
            }
LABEL_410:
            variable_value_result = *(unsigned __int8 *)(reference_count + 5);
            if ( (variable_value_result & 3) == 1 )
            {
              *variable_id = variable_ptr;
            }
            else if ( (variable_value_result & 3) == 2 )
            {
              position_value = (__int16)variable_ptr + *variable_id;
              if ( position_value >= -30000 )
              {
                if ( position_value > 30000 )
                  LOWORD(position_value) = 30000;
                *variable_id = position_value;
              }
              else
              {
                *variable_id = -30000;
              }
            }
            operation_flags = ((variable_value_result >> 2) & 3) - 1;
            if ( operation_flags )
            {
              variable_value_added = operation_flags - 1;
              if ( variable_value_added )
              {
                if ( variable_value_added != 1 || *variable_id >= *(__int16 *)(reference_count + 9) )
                  goto LABEL_333;
              }
              else if ( *variable_id <= *(__int16 *)(reference_count + 9) )
              {
                goto LABEL_333;
              }
            }
            else if ( *variable_id != *(_WORD *)(reference_count + 9) )
            {
              goto LABEL_333;
            }
            if ( !*(_WORD *)(reference_count + 1) )
              goto LABEL_333;
            comparison_type = object_data_ptr[68];
            comparison_type_sub = *(unsigned __int16 *)(reference_count + 1);
            *(_DWORD *)(v0 + 48) = comparison_type_sub;
            LOWORD(comparison_type_sub) = *(_WORD *)(39 * comparison_type_sub + comparison_type + 32);
            sprite_data_base_ptr5 = *(unsigned __int8 *)(reference_count + 3);
            *(_DWORD *)(v0 + 44) = (unsigned __int16)comparison_type_sub + sprite_data_base_ptr5 - 1;
            *(_DWORD *)(v0 + 44) = (unsigned __int16)comparison_type_sub + sprite_data_base_ptr5;
            goto LABEL_133;
          case 0x20:
            if ( game_rand() % (*(unsigned __int16 *)(reference_count + 1) + 1) <= *(unsigned __int16 *)(reference_count + 3)
              || !*(_WORD *)(reference_count + 6) )
            {
              goto LABEL_332;
            }
            v0 = g_object_data_ptr;
            target_attack_state_id4 = object_data_ptr[68];
            frame_offset_result = *(unsigned __int16 *)(reference_count + 6);
            *(_DWORD *)(g_object_data_ptr + 48) = frame_offset_result;
            LOWORD(frame_offset_result) = *(_WORD *)(39 * frame_offset_result + target_attack_state_id4 + 32);
            sprite_data_base_ptr6 = *(unsigned __int8 *)(reference_count + 8);
            *(_DWORD *)(v0 + 44) = (unsigned __int16)frame_offset_result + sprite_data_base_ptr6 - 1;
            *(_DWORD *)(v0 + 44) = (unsigned __int16)frame_offset_result + sprite_data_base_ptr6;
            goto LABEL_133;
          case 0x23:
            target_attack_state_id5 = *(char *)(reference_count + 2);
            frame_offset_result2 = *(char *)(reference_count + 3);
            *(_DWORD *)(v0 + 84) = *(unsigned __int8 *)(reference_count + 1);
            *(_DWORD *)(v0 + 68) = target_attack_state_id5;
            sprite_param1 = *(char *)(reference_count + 4);
            *(_DWORD *)(v0 + 72) = frame_offset_result2;
            *(_DWORD *)(v0 + 76) = sprite_param1;
            if ( *(_DWORD *)(v0 + 84) == 4 )
              *(_DWORD *)(v0 + 80) = *(char *)(reference_count + 5);
            else
              *(_DWORD *)(v0 + 80) = 0;
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          case 0x24:
            if ( !CheckMotionInputCommand(reference_ptr + 16 * *(_DWORD *)(v0 + 44)) )
              goto LABEL_332;
            v0 = g_object_data_ptr;
            sprite_param2 = *(_WORD *)(reference_count + 1);
            *(_DWORD *)(g_object_data_ptr + 48) = sprite_param2;
            sprite_param3 = *(_WORD *)(39 * sprite_param2 + object_data_ptr[68] + 32);
            target_attack_state_id6 = *(unsigned __int8 *)(reference_count + 3);
            *(_DWORD *)(v0 + 44) = sprite_param3 + target_attack_state_id6 - 1;
            *(_DWORD *)(v0 + 44) = sprite_param3 + target_attack_state_id6;
            goto LABEL_133;
          case 0x25:
            if ( *(_BYTE *)(v0 + 337) )
            {
              animation_frame_count_result3 = *(unsigned __int8 *)(v0 + 337) - 1;
              if ( !*(_BYTE *)(reference_count + 3) || !*(_BYTE *)(reference_count + 4) )
              {
                *(_BYTE *)(v0 + 337) = 0;
                sprite_data_base_ptr7 = 101 * animation_frame_count_result3;
                sprite_index = *(_DWORD *)(v0 + 44) + 1;
                g_sprite_data_active_flags[4 * sprite_data_base_ptr7] = 0;
                *(_DWORD *)(v0 + 44) = sprite_index;
                goto LABEL_133;
              }
            }
            else
            {
              animation_frame_count_result3 = 0;
              state_flags_temp2 = g_sprite_data_active_flags;
              while ( *state_flags_temp2 )
              {
                state_flags_temp2 += 404;
                ++animation_frame_count_result3;
                if ( (int)state_flags_temp2 >= (int)&g_sprite_data_array_end )
                  goto LABEL_446;
              }
              g_sprite_data_active_flags[404 * animation_frame_count_result3] = 1;
              *(_BYTE *)(v0 + 337) = animation_frame_count_result3 + 1;
            }
LABEL_446:
            if ( animation_frame_count_result3 == 100 )
            {
              AddDebugLogMessage(g_debug_error_message, (int)&g_debug_error_color);
              goto LABEL_332;
            }
            sprite_data_index = 404 * animation_frame_count_result3;
            g_sprite_data_frame_counters[sprite_data_index] = 0;
            g_sprite_data_command_ptrs[sprite_data_index] = reference_count;
            g_sprite_data_state_flags[sprite_data_index] = 0;
            memset((char *)&g_sprite_data_buffer + 1616 * animation_frame_count_result3, 0, 0x640u);
            ++*(_DWORD *)(v0 + 44);
            goto LABEL_133;
          default:
            goto LABEL_333;
        }
      }
      sprintf(
        Buffer,
        "ScriptMainLoopError %d %d - nd:%d step:%d",
        *(_DWORD *)(v0 + 342),
        *(_DWORD *)(v0 + 346),
        loop_counter,
        *(_DWORD *)(v0 + 44) - *(unsigned __int16 *)(39 * loop_counter + object_data_ptr[68] + 71));
      AddDebugLogMessage(Buffer, 8421631);
      goto LABEL_450;
    }
  }
}