# ![][image1]

# Standard Codeblocks

## \[I\] \- Image

![][image2]

### Wait

Entry: 0-65535

Defines how many frames the Image will be active.  
1 Frame is 10ms

### I

Entry: 0-8191

Defines which frame is being used for this animation.

### Axis

Entry One (X-Position): \-30,000 to 30,000  
Entry Two (Y-Position): \-30,3000 to 30,000

The first entry defines the image’s position on the X-Axis. Each increment is one “unit.” Positive X is to the right, negative X is to the left.  
The second entry defines the same but for the Y-Axis. Positive Y is down, negative Y is up.

### Checkboxes

| Turn X | Horizontally flips the image. |
| :---- | :---- |
| **Turn Y** | Vertically flips the image. |
| **Ignore the direction** | Makes the image’s left/right direction not flip if it’s being called by a “right-side” player. |

## \[M\] \- Movement

![][image3]

### Move

Entry (X): \-300.00 to \+300.00 (Two decimal places)  
Checkbox: Stop  
Entry (Y): \-300.00 to \+300.00 (Two decimal places)  
Checkbox: Stop

| Move X | Applies a horizontal speed to the entity. Positive is right, negative is left. |
| :---- | :---- |
| **Move Y** | Applies a vertical speed to the entity. Positive is down, negative is up. |

Checking stop will result in the entry being ignored entirely. Existing speed will not change.

### Gravity

Entry (X): \-300.00 to \+300.00 (Two decimal places)  
Checkbox: Stop  
Entry (Y): \-300.00 to \+300.00 (Two decimal places)  
Checkbox: Stop

| Gravity  X | Adds this much horizontal speed to the entity every frame. Positive is right, negative is left. |
| :---- | :---- |
| **Gravity  Y** | Adds this much vertical speed to the entity every frame.  Positive is down, negative is up. |

Checking stop will result in the entry being ignored entirely. Existing speed will not change.

### Selection

| Replace | Replaces the existing Move and Gravity values with the ones in the entries. |
| :---- | :---- |
| **Add** | Add the existing Move and Gravity values with the ones in the entries. The total becomes the entity’s new X/Y speed and acceleration. |

## \[FD\] \- Defense Frame

![][image4]

### M. Number

Entry: 0-19

Hurtbox identifier. There can only be 20 hurtboxes on a character. Creating a new hurtbox with identifier \#0 will remove overwrite the previous hurtbox with identifier \#0.

### Checkboxes

Three Checkboxes: Striking, Doing, Throwing

| Striking | This box has collision detection. (Pushbox) |
| :---- | :---- |
| **Doing** | This box is a hurtbox. (Can be dealt damage directly) |
| **Throwing** | This box can be thrown (\[DS\]: While throw do will detect this box) |

### Ratio

Entry: 0-255, only usable if Doing is checked.

The percentage multiplier for damage this hurtbox takes. A ratio of 0 results in 1 damage.

### XY Entries

Entry (Top-Left): \-30,000 to 30,000  
Entry (Top-Right): \-30,000 to 30,000  
Entry (Bottom-Left): 0 to 30,000  
Entry (Bottom-Right): 0 to 30,000

| Top Left (X-Position) | How many units horizontally to move the box. Positive is right, negative is left. |
| :---- | :---- |
| **Top Right (Y-Position)** | How many units vertically to move the box. Positive is down, negative is up. |
| **Bottom Left (Box X-Radius)** | Increase the X-Radius by this many units. Ex. A value of 1 is 2 units wide. |
| **Bottom Right (Box Y-Radius)** | Increase the Y-Radius by this many units. Ex. A value of 1 is 2 units tall. |

## \[FA\] \- Attack Frame

![][image5]

### M. Number

Entry: 0-19

Hitbox identifier. There can only be 20 hitboxes on a character. Creating a new hitbox with identifier \#0 will remove overwrite the previous hitbox with identifier \#0.

### Power

Entry: 0-255

Determines the amount of damage the move will deal.

### Checkboxes

Eight checkboxes: Cancel, No Detection, Cont. Hits, No Sky Decision, Guard Fail, While Guard, While Receiving, Shave  
“No Sky Decision” and “While Guard” names are likely mixed up in-engine. Newer translations probably fix this.

| Cancel | This hitbox can use a \[C\] codeblock cancel on hit. |
| :---- | :---- |
| **No Detection** | This hitbox does not hit a player that is currently on the ground. |
| **Cont. Hits** | This hitbox can combo from a previous hitbox without the need to use a \[C\] codeblock cancel. |
| **No Sky Decision** | This hitbox can’t hit a player that’s currently in blockstun. |
| **Guard Fail** | This hitbox can’t be blocked. |
| **While Guard** | This hitbox does not hit a player that is currently in the air. |
| **While Receiving** | This hitbox does not hit a player that is currently in hitstun. |
| **Shave** | Move deals X% chip damage on block. X is defined in the character’s basic settings as “S.ratio.” Minimum chip damage is 1\. |

### XY Entries

Entry (Top-Left): \-30,000 to 30,000  
Entry (Top-Right): \-30,000 to 30,000  
Entry (Bottom-Left): 0 to 30,000  
Entry (Bottom-Right): 0 to 30,000

| Top Left (X-Position) | How many units horizontally to move the box. Positive is right, negative is left. |
| :---- | :---- |
| **Top Right (Y-Position)** | How many units vertically to move the box. Positive is down, negative is up. |
| **Bottom Left (Box X-Radius)** | Increase the X-Radius by this many units. Ex. A value of 1 is 2 units wide. |
| **Bottom Right (Box Y-Radius)** | Increase the Y-Radius by this many units. Ex. A value of 1 is 2 units tall. |

## \[R\] \- Reaction

![][image6]

Hits: What happens to the opponent if this move hits them.  
	Stand: Selects a hit junction for standing.  
	Crouched: Selects a hit junction for crouching.  
	Air: Selects a hit junction for when the opponent is in the air.

Guard: What happens to the opponent if they block the move.  
	Stand: Selects a hit junction for standing.  
	Crouched: Selects a hit junction for crouching.  
	Air: Selects a hit junction for when the opponent is in the air.

If a move is “Guarded” it won’t deal damage unless that hit junction has the “Doing” selection enabled..

## \[S\] \- Sound

![][image7]

### Sound Select

Selection: File  
Checkbox: Loop to Point

Allows you to choose a sound from the list of those inserted into the character.  
Selecting “None” stops all sounds playing for that character.

Checking the Loop box loops the sound.

All other features are only relevant to the editor- not the character.

## \[C\] \- Cancel Condition

![][image8]

### Cancelation

Select between 3 options:  
Fail, Hit, Uncod.

| Fail | Makes you unable to cancel the associated skill. |
| :---- | :---- |
| **Hit** | Can cancel on hit or block. Only for the player, Objects containing hitboxes won’t cause this to trigger. Requires the “Cancel” checkbox to be checked in an \[FA\] to trigger. |
| **Uncod.** | Can always cancel. |

### CancelCondition

Selection between 2 options:  
Level  
Skill

#### Level

Entry (Empty): 0-255  
Entry (Between): 0-255  
Between is ALWAYS \>= Empty

All moves have an Sk Level associated with Cancel. This is part of the inherent elements of a skill. ![][image9]

If you meet the following three requirements:  
• You meet the cancel condition.  
• There's a skill with an appropriate Sk value that's equal to or between your two entry values.  
• That skill is mapped to an input on the Commands screen

You can do the input, and cancel into the appropriate skill.

#### Skill

\[Skill Selection\]  
Notably CAN'T  directly select a codeblock in the middle of the skill, unlike basically any other type of jump.

If you meet the following three requirements:  
• You meet the cancel condition.  
• A skill is selected in the Skill box.  
• That skill is mapped to an input on the Commands screen.

You can do the input, and cancel into the skill.

Extra Notes on \[C\]:  
\[C\] carries over between skills if you jump between them.

Only the most recent \[C\] is considered, previous ones are discarded.

\[C\] being set to Hit/Uncod. is ignored if you’re inside a Hit Junction (you’ve gotten hit and are in hitstun).\*  
\*Not 100% on this one but I don’t know any exceptions to the rule.

## \[COLOR\] \- Color Modification {#[color]---color-modification}

![][image10]

Selection between 5 options: Normal, 50% \+ 50% Alpha, A \+ B Addition, A \- B Subtract, a % b Alpha

Hard to parse out what 2DFM is doing exactly with the Color blocks, but I assume it follows the universal standards for RGB blending. I couldn’t tell you precisely what the maths here are except they probably follow the same rules as you’d find in any modern digital art tool.  
[https://en.wikipedia.org/wiki/RGB\_color\_model](https://en.wikipedia.org/wiki/RGB_color_model)

| Normal |  |
| :---- | :---- |
| **50 \+ 50% Alpha** | Color modification \+ a 50% opacity applied.I use this for changing the color of a sprite, not with precision but for temporary states indicating a power up or something. |
| **A \+ B Addition** | [https://en.wikipedia.org/wiki/Blend\_modes\#Addition](https://en.wikipedia.org/wiki/Blend_modes#Addition)I use this most often for hit sparks and other ‘glow’ effects. |
| **A \- B Subtract** | [https://en.wikipedia.org/wiki/Blend\_modes\#Subtract](https://en.wikipedia.org/wiki/Blend_modes#Subtract)I don’t really use this, I could see it used for stand-like ‘negative’ effects. |
| **a % b Alpha** | Color mod \+ set your own level of opacity.Useful for animating an image fading in or out. |

Entry (R): \-32 to \+32  
Entry (G): \-32 to \+32  
Entry (B): \-32 to \+32  
Entry (A): \-32 to \+32, can only be used when **a % b Alpha** is selected

## \[O\] \- Object

![][image11]

Calls an object that acts as a separate entity from the player from a chosen point in the skill list.  
The called object is then created on the next frame.  
Usually used for VFX or Projectiles.

### Position

Entry (X): \-30,000 to 30,000  
Entry (Y): \-30,000 to 30,000

| X | Relative X-Position of object, compared to what’s creating it. |
| :---- | :---- |
| **Y** | Relative Y-Position of object, compared to what’s creating it. |

Note that you can create objects in objects, and doing so makes these values relative to the “parent object”

### Object

\[Skill selection\]  
\[M. number\]

The skill selection chooses from where the object is being loaded.  
M. Number is the object’s number. Only one object per M. Number can exist at a time. Loading an object with the same M. Number again will result in the previous object unloading. This includes loading a null object with “None” which deletes the object entirely and does not create a new one.

**Checkboxes:**

| Parent | Created object moves with Parent. This nullifies any XY speed that object has inside its own code. |
| :---- | :---- |
| **UnCond** | Object is always created, ignoring the M. Number limits. Disabled M. Number and It’s Out when used. Creating too many entities eventually results in a crash. |
| **Shadow** | Object can cast a shadow? (Unsure, I’ve never had this set up, nor seen any proper documentation or examples of this option.) |
| **XY** | Uses the absolute XY values of the map, rather than the relative values from the parent. |

### Depth

| In | Generally puts the created object under other entities visually. |
| :---- | :---- |
| **Out** | Generall puts the created object above other entities visually. |
| **Point** | Visual layer is determined by the Depth Ap number. |

Depth AP: 0 to 127 (Unusable unless Depth is set to Point)

Depth AP determines which layer is used for a Point of Depth. 0 is at the bottom, 127 is at the top.  
I’m not quite sure where In and Out are on this system, but I believe 0 is a layer below “In” and 127 is a layer above “Out”

### It’s Out

\[Skill Selection\]

When you attempt to create an object with the same M. Number with one already active, you move to the selected skill instead. Acts like a conditional jump, similar to V’s Cond branch.

## \[E\] \- End

![][image12]

Ends the skill. For characters, notably different from a skill ending by running out of actions in one way- Standard branching functions like SG can branch directly to E to end a skill. You can't point something like SG to an area "after" a skill is over.

For Objects, deletes the Object. Objects can’t use and End codeblock to exit an SC or SF codeblock, they must instead reach the end of a skill to exit those functions.

# Condition Forks

## \[V\] \- Variables

![][image13]

### \# calculate

Selection between 3 Var Types:  
Task (A-P)  
Char (A-P)  
System (A-P)  
Loads the chosen value from memory.

Task Variables are localized entirely within an entity’s skill. Essentially all skills have 16 variables that they can individually remember for the duration of a round. These variables are saved to the entity, so objects aren’t able to read task variables set by players. When an object is deleted, any task variables it had stored are also removed, and can’t be read by a new object. 

In addition, hitstun is treated as a single “move” so Task Variables carry between hit states during the entire duration of the combo. If they’re not reset when the combo ends, they can carry from one combo to another.

Task variables are set to 0 on round start.

Character variables are set to the character in its entirety. Any skill and any object that player creates can read character variables. So each player has 16 variables that they can read or write to with any skill or object.   
Character variables are set to 0 on round start.

System Variables are true globals. Anything can read these and write to them.  
System Variables are set to 0 when launching the game, and are never reset.

Entry: \-30,000 to 30,000  
OR  
Use This: Can load any other variable, including a few extras:

| Data: X Coor | Entity's X-Pos |
| :---- | :---- |
| **Data: Y Coor** | Entity's Y-Pos |
| **Data: Map X Coor** | X-Coordinate of the Camera’s Center |
| **Data: Map Y Coor** | Y-Coordinate of lowest visible pixel |
| **Data: Parent X** | X-Position of this object's parent\* |
| **Data: Parent Y** | Y-Position of this object's parent\* |
| **Data: Time** | Current round timer |
| **Data: \# of Rounds** | Starts at 1, increments every round |

\*Parent is the entity that created the object

Selection Bubbles:

| No Calculation | Does nothing. |
| :---- | :---- |
| **Replace** | Store the entered value or "use this" value to the selected Var. |
| **Add** | Add the entered value or "use this" value to the selected Var. |

### Cond Branch

Selection between 4 options:  
No Cond Branch, It's the same, It's Above, It's Below

Entry: \-30,000 to 30,000  
\[Skill Selection\]

Selection Bubbles:

| No Cond Branch | No comparison Check |
| :---- | :---- |
| **It's the same** | (Jump) If Var's value is the same as branch's entered value. |
| **It's above** | If Var's value is greater than branch's entered value (NOT EQUALS) |
| **It's Below** | If Var's value is less than branch's entered value(NOT EQUALS) |

If the condition is met, you jump to where the Skill Selection points to.

## \[DS\] \- Detect Skill

![][image14]

### Divergence

Select between 7 types of \[DS\]:  
Not, Landing, Attack Hits, Defending, Hit the Wall, in offset, While throw do  
\[Skill Selection\]

| Not | Don't Jump (default) |
| :---- | :---- |
| **Landing** | (Jump) when this entity collides with the ground |
| **Attack Hits** | When this entity's hitbox collides with a hurtbox that *doesn't* block. |
| **Defending** | When this entity's hitbox collides with a hurtbox that *does* block. |
| **Hit the Wall** | When this entity collides with the CAMERA wall. |
| **in offset** | When this entity's hitbox collides with another hitbox. |
| **While throw do** | When this entity's hitbox collides with a hurtbox (excluding throw invulnerable hurtboxes). |

If the condition is met, you Jump to where the Skill Selection points to.

\[DS\] transfers between jumps- so it's a permanent flag added to the move until the move in its entirety ends.

You can have multiple different \[DS\] types active at the same time.

Adding a new \[DS\] of the same type will override the old one.

The DS triggering also disables that specific \[DS\] check flag. (\[DS\] Attack Hits triggering will remove the DS Attack Hits flag for the rest of the move)

A DS triggering does not disable \*other\* \[DS\] flag types. (\[DS\] Attack Hits triggering will NOT remove a \[DS\] Landing flag from the move)

Setting a \[DS\] to a value other than "Not" but leaving the Skill Selection empty will disable that specific check's \[DS\] flag. (DS Landing \[Skill\] can be disabled by \[DS\] Landing None)

## \[DB\] \- Basic Divergence

![][image15]

### Setting

Selection between 8 options  
its not, Guarding, Standing, Crouching, If Forward is tapped, if Back is tapped, if Up is tapped, if Down is tapped

| its not | No check |
| :---- | :---- |
| **Guarding** | If this entity is on the ground |
| **Standing** | If this entity is in a standing animation |
| **Crouching** | If this entity is in a crouching animation |
| **If Forward is tapped** | If the player associated with this entity is holding forward. |
| **If Back is tapped** | If the player associated with this entity is holding backward. |
| **If Up is tapped** | If the player associated with this entity is holding up. |
| **If Down is tapped** | If the player associated with this entity is holding down. |

Diagonals count for either, so Down+Forward would be a successful check for Forward is tapped and/or Up is tapped.

### Call Ahead

Selection between 2 options:  
formed, If Fialed  
\[Skill Selection\]

| formed | Jump if the chosen Setting's condition was met |
| :---- | :---- |
| **If Fialed** | Jump if the chosen Setting's condition was NOT met |

its not \+ formed is "don't Jump"  
its not \+ fialed is "always Jump"  
An object that can't stand or crouch will always fail Standing/Crouching checks

## (R) \- Random

![][image16]

Entry (Total Numbers): 0-65535  
Entry (When its above): 0-65535  
\[Skill Selection\]

RNG rolls a number between 0 and "Total Numbers"  
If the result is above "When its above" branch to the skill selection.

Not sure if it's important, but RNG is completely fixed in 2DFM. The game always starts on a specific seed on boot. Not sure on any RNG specifics beyond that.

## \[COM\] \- Command Divergence

![][image17]

Entry (Command Time): 0-255  
\[Skill Selection\]  
\[Command Inputs x5\]

Command Time is how many frames of buffer you have. It doesn't care if your inputs were already "used" for another input/command/anything. It'll check ALL inputs within the buffer window.

Skill Selection is where you will jump to if you do the Command Inputs correctly.

Command inputs let you choose between 1-5 consecutive command inputs within the window.  
Options are cycled through in this order:  
Free (any directional input)  
Any single direction (Keypad notation, in order: 5, 6, 3, 2, 1, 4, 7, 8, 9\)  
Left \+ Any other directional input  
Up \+ Any other directional input  
Right \+ Any other directional input  
Down \+ Any other directional input

Not filling out blocks are basically "Free space" to allow extra leniency in inputs. Essentially allowing Down, (any), Down+Forward, (any), Forward instead of requiring Down, Down+Forward, Forward in that order with no other inputs between them.

![][image18]

# Script Movement

## \[SG\] \- GoTo

![][image19]

\[Skill Selection\]

Jumps to the selected skill and codeblock.

## \[SC\] \- Call

![][image20]

\[Skill Selection\]

Jump and return.

\[SC\] creates a return flag, then you jumps to the selected location.

When the current action would end (either reaching the end of a skill, or reaching an E), it returns to the \[SC\] codeblock and continues the skill from there.

There can only be one active return flag per entity. Using \[SC\] a second time while inside an \[SC\] call will move the flag to the new SC, removing the old one. Triggering a \[DS\] while in an \[SC\] call will also clear the \[SC\] Flag, preventing you from returning.

## \[SF\] \- Loop

![][image21]

\[Skill Selection\]

Entry (Loop): 0-255

SF creates a return flag, then jumps to the selected skill. It then iterates through that skill a number of times, equal to the value in the entry box. On completion of all the loops, it returns to the \[SF\] codeblock, continuing the skill from there. Like \[SC\], there can only be one return flag active per entity, and the flag can be escaped/cleared in the same way. So an \[SC\] will clear \[SF\]’s return flag, and vice-versa.

# Successful Partner Hit Management

## \[RC\] \- Change Shape’s Condition (Common Image)

![][image22]

### Position

Entry (X): \-9,999 to 9,999  
Entry (Y): \-9,999 to 9,999

| X-Position | The horizontal displacement of the opponent’s image from the entity that called this. Positive is right, negative is left. |
| :---- | :---- |
| **Y-Position** | The vertical displacement of the opponent’s image from the entity that called this. Positive is down, negative is up. |

### Common Image

Selection: List of Common Images

Allows you to pick from a list of user-defined common images. None is blank.

### Depth

Selection between two options: In, Out  
Three checkboxes: Turn X, Turn Y, Same

| In | The opponent’s sprite layer will be moved to under the entity’s. (The opponent’s sprite will appear underneath your own.) |
| :---- | :---- |
| **Out** | The opponent’s sprite layer will be moved to above the entity’s. (The opponent’s sprite will appear on top of your own.) |
| **Turn X** | Horizontally flips the opponent’s sprite. |
| **Turn Y** | Vertically flips the opponent’s sprite. |
| **Same** | If the entity that called this is moving, the opponent’s sprite will move in sync with the entity. |

Sample Character is only used for the engine. It’s not relevant for the character files.

## \[RP\] \- Change Skill (Partner: Script Mod)

![][image23]

### Position

Entry (X): \-9,999 to 9,999  
Entry (Y): \-9,999 to 9,999

| X-Position | The horizontal displacement of the opponent’s image from the entity that called this. Positive is right, negative is left. |
| :---- | :---- |
| **Y-Position** | The vertical displacement of the opponent’s image from the entity that called this. Positive is down, negative is up. |

### HitJunction

Selection: List of Hit Junctions

Allows you to pick from a list of user-defined common images. None causes the opponent to be teleported to the coordinates but not pick a hitstun, resulting in them immediately standing.

### Depth

Selection between two options: In, Out  
One checkbox: Turn X

| In | The opponent’s sprite layer will be moved to under the entity’s. (The opponent’s sprite will appear underneath your own.) |
| :---- | :---- |
| **Out** | The opponent’s sprite layer will be moved to above the entity’s. (The opponent’s sprite will appear on top of your own.) |
| **Turn X** | Horizontally flips the sprite AND the knockback. This uniquely allows RPs to mirror the knockback of a HitJunction without creating any additional skills, which can’t be done anywhere else. |

# Gauge

## \[GS\] \- Special Gauge Check

![][image24]

### Setting

Entry (Special Guage): 0-9  
Selection: When Little, When Alot  
Entry (Add to advance): \-1,000 to 1,000

| When Little | Succeeds if your current meter is less than the "Special Guage" value |
| :---- | :---- |
| **When Alot** | Succeeds if your current meter is greater than the "Special Guage" value |

Add to Advance: If the above check was successful, Add or subtract this much meter from this player.

### If it failed

\[Skill Selection\]

If the above check failed, jump to the selected skill instead.

## \[GL\] \- Life Gauge Check

![][image25]

### Setting

Entry (Life Guage): 0-1000  
Selection: When little, When Alot

| When little |  Succeeds if your current life is less than the entered value. |
| :---- | :---- |
| **When Alot** | Succeeds if your current life is greater than the entered value. |

### If it Failed

\[Skill Selection\]

If the above check failed, jump to the selected skill.

## \[GC\] \- Change Gauge Value

![][image26]

### Yours

Entry (Life Gauge): How much to increase or decrease this player’s current health by.  
Entry (Special Gauge): How much to increase or decrease this player’s current special gauge by.

Both values are affected by caps programmed into basic character attributes, and are floored at 0\.  
Note that this increases Special Gauge. X amount of special Gauge makes a special stock.  
Also worth noting that this only cares about the player, not the entity. Objects can use \[GC\] just fine, it applies to the players.

### His

Entry (Life Gauge): How much to increase or decrease the opponent’s current health by.  
Entry (Special Gauge): How much to increase or decrease the opponent’s current special gauge by.

Works the same as “yours” just for your opponent.

# Special Effect

## \[EB\] \- Background Effect

![][image27]

### Palette Flash

EB has the same color entry table as the COLOR codeblock. Lacks the selection though, so the color table is only ever treated the same as “Normal.” Despite the inclusion of an alpha entry, I don’t think it’s actually used here.   
Read more on the color table [here](#[color]---color-modification).

Entry (Duration): 0-65535  
The duration is the number of frames the color effect is interpolated over. The color effect doesn’t end after this amount of frames, you need to use a second EB to modify it again.

Selection between four options: Unused, Smooth Fading, Chika Chika Fading, Random

| Unused | Disables the Duration and Color Table entirely. No color effect. |
| :---- | :---- |
| **Smooth Fading** | Starts from RGB 0, 0, 0 and interpolates color changes until the last frame reaches the value entered in the color table. The longer the duration, the slower the fade. |
| **Chika Chika Fading** | Flashes between RGB 0, 0, 0 and the interpolated “Smooth Fading” value every frame. So Frame 1 would be RGB 0, 0, 0 and frame 2 would be the same value as frame 2 from Smooth Fading. Once the fade reaches the entered value, it stops flashing, and the color stays the entered value. |
| **Random** | Randomly flashes colors “between” RGB 0, 0, 0 and the entered values for the duration. |

Four checkboxes: Player, BG, Enemy, System

| Player | (Color changes apply to) All entities associated with this player. |
| :---- | :---- |
| **BG** | The stage. |
| **Enemy** | All entities associated with the opponent. |
| **System** | Entities controlled by the game system. |

### Shake BG

Selection (X) between five choices: Unused, Fade Out Shake, Fade In Shake, Fixed Shake, Random  
Entry (Duration): 0-255  
Entry (Shake): 0-255

Selection (Y) between five choices: Unused, Fade Out Shake, Fade In Shake, Fixed Shake, Random  
Entry (Duration): 0-255  
Entry (Shake): 0-255

Shake BG alters the location of where entities and background graphics appear in a back-and-forth shaking motion. It does this uniformly, so everything is effected in the same way. Does not shake Game System elements. Does not effect the location of hitboxes or hurtboxes. 

There is no transition for shaking, the images “teleport” between two locations. Shaking always alternates between right \-\> left (repeat) and down \-\> up (repeat).

| Unused | Disables the duration entries entirely. No shake effect. |
| :---- | :---- |
| **Fade Out Shake** | Shakes back and forth starting at {Current Position}+X and {Current Position}-X. X interpolates towards 0 based on the duration of the shake.  |
| **Fade In Shake** | Shakes back and forth starting from {Current Position}±0 and interpolates towards {Current Position}+X and {Current Position}-X based on the duration of the shake. |
| **Fixed Shake** | Shakes back and forth at {Current Position}+X and {Current Position}-X.  |
| **Random** | Shakes randomly between {Current Position}+{Random Value ≤ X} and {Current Position}-{Random Value ≤ X}. |

## \[PS\] \- Player Stop (Time Stop)

![][image28]

Entry (Your Time): 0-255  
Entry (His Time): 0-255

The entry value is how many frames to stop the chosen entity. 0 means no time stop.

| Your Time | Stops time for the entity that called the PS. Only stops time for the player if called by the player. Only stops time for an object if called by an object. |
| :---- | :---- |
| **His Time** | Stops time for the enemy player. |

## \[AI\] \- After Image

![][image29]

Entry (num): 0-255  
Entry (Time): 0-255

After images make an image copy of the entity’s current frame, makes a copy of it, and creates that as a still image.

Num controls how many of these after images can exist at once. When the capacity is reached, new AIs delete the oldest one.

Time controls how many frames the entity waits between creating images. The first image is always created on frame 1\.

### Color Table

AI has the same color entry table and color selection menu as the COLOR codeblock.  
Read more on the color table [here](#[color]---color-modification).

Second Selection Menu: Select between unused, fixing, smooth fading, chika chika fade, and random.

| Unused | (After images) use only the color selection menu, and ignores the color entry table entirely. |
| :---- | :---- |
| **Fixing** | Uses the exact same color changes as the COLOR codeblock. |
| **Smooth Fading** | Uses the color selection menu’s setting for frame one, but with RGBA set to 0, 0, 0, 0\. The final frame of the after image uses the RGBA equal to what you set in the color entry table. The game then interpolates a smooth fade between the two, based on the Time setting. A longer lasting after image causes a slower fade, essentially. |
| **Chika Chika Fade** | Uses the color selection menu’s settings. Flashes between RGBA initially set to 0, 0, 0, 0 and the RGBA set in the color entry table (probably every frame?). This also interpolates the color like Smooth Fading- the RGBA 0, 0, 0, 0 color will approach the values set to the table each frame. This fade is also slower the longer the AI is. |
| **Random** | Flashes a random value “between” RGBA 0, 0, 0, 0 and the RGBA set in the color entry table every frame. |

[image1]: <data:image/png;base64,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>

[image2]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAP8AAADjCAIAAADFflV9AAALMUlEQVR4Xu3dXZqbOBaAYe/GuZ+NeBu9AmcvyWS8m8oi+q5ukie/O/AYZIR0JCEhMJY533uRLgOmLOkzoSqVzuF6vf4F9u7Pnz+/f//+9evXz58/f/z48f3792/fvh2oHxpQP/Saqv8C7Nr/el++fPlv7/Pnz58+faJ+qED90KvR+q8OuQ9YSaP1X4Y3gNwKrIf6odc69d/vUVbtdd2zASHqh17r1H95QKzhCc0WIbXXeV5kr3tAdCM0eOH63WPsB+6R0Yflu7B7r1d/9ANBbHcfpj5ObcGO7ap+89Cy28Wu6EaX8zzs2X7qT30cPsxuhxLr1G8yWjem8Gx2y8QH4kj3oSt6GLTZYf3mY/Hwfopgi9gbHowdW6f+1ZkKU2mGH4inWOIY90jx0HIOxM41Wv+6RN9XEkdPRf1AFPVDL+qHXlP1A/s29bfagX2jfuhVX//peH77ezkdTpfbr93HobfzKbpdq9tsHbr5QiPq6z8fT5e389HUX7KkqfeIIm/dpMmNeJr6+rvku/qP50vZmlI/9TdmSf3H8/l0Op1v9ZsbnP439sNhbHy887nv8nfrQ/1tqa+/S7tzewP0F/VhZZ2bff++n2s/9TcmU/+/MWbXbSWPx9Ptwt/1f1vSt7cubXMvVFa/PO8rmxiRM+JM/fKZuyYH/wz5+v3jHd3djLn1N9/H6O9uuq8C1F377SxNTVcnU78euYnayOL6u//cb3xM3ArvfKh/rtxEbWRB/ffeRf32XdAfQf0e6r/LTdRGltdvV9R8X+d4+xp4qFz8aVfwPaG9KK2fP+0aZCZqKwvqx6C0fgwamSjqXwH1z9XIRFH/Cqh/rkYmivpXQP1zNTJR1L8C6p+rkYmi/hVQ/1yNTFRl/fIvSKoR/QmobP3yLC8rOvwKqYnaWH39/+jz/v4eXf6S+uW5XlBq+BVSE7WxRfVflUktf2H98nSvJjX8CqmJ2hj1z5BafuqfKzVRG6P+q/17NxPMkanl30H9csAD95jU8CukJmpj1J9ceJc5MrX8r16/HK3PHpYafoXURG2M+se1lzsCqeV/6fqdzr0ZCLenhl8hNVEbo37qTw5f7EoNv0JqojZG/aW/71/Ty0/9c6UmamPUn6n/UHDjS/1zpSZqY9SfZNc+u/zUP1dqojZG/VMKl5/650pN1Maof0rh8lP/XKmJ2hj1J9m1zy7/Puq3w0xtTw2/QmqiNkb92r/qveZmwB6WGn6F1ERtjPpL1/6aXv5Xr/+angT3mNTwK6QmamPUP0Nq+XdQf4nU8CukJmpj1D9Davmpf67URG2svv53laLLX1K/PNFrig6/QmqiNlZZv3dNUEbORUH98hSvTI6tSmqiNlZZP1zZ+iE0MlHUvwLqn6uRiaL+FVD/XI1MFPWvgPrnamSiqH8F1D9XIxNF/Sug/rkamah8/SjBdM01Nvg8mfqBHaN+6EX90Iv6oRf1Qy/qh17UD70W1n85Tf3ry+Jfqwbasqz+t/Nxh//0OrRYVv/t0n+YuvgDLVtU/+3SfzqdzMW/+22gfx/c3hHDbwfunU+3v8N7Bc1YUv8t6OP50v3SJ969Fy79L+MBtv5h++X+ZgGeL1O//NGk3vBc8yWv84WvvBFy6u92zbvsy88KLCYay9fvH+8Y7mUO4xe+4ltA4ns+8s0BbCmMeUH9w02Mvdnp7oSOw32Q2eBc+80x098iBR4njLm+/vEO//42GG6Exvy9+s1m7vvxLGHM9fXbr3bNV7/n4Vs9/aNhh3Pn09/3cOeDpwljrq8feC1hzNQPLcKYqR9ahDFTP7QIY6Z+aBHGTP3QIoy5sv4LymR/Yko+AQnZmcwKY66v/x/klPxzD8xkiZKZzApjXlT/FZNK1oyZLFEyk1lhzNT/QCVrxkyWKJnJrDDm9es3P/Ypt+7O/cdbA+4xJWvGTPrzN3KPKZnJrDBm6q/hL5NkDytZM2Zygj2sZCazwpipf7bo8kS3l6wZMxkOM9xeMpNZYcwr1O++1pRhXHswMSixq2TNmMnooMSukpnMCmOm/tkmBiV2lawZMxkdlNhVMpNZYcwr1C+kxrMbYmEmdpWsGTMZHaPYVTKTWWHM1D+bWJiJXSVrxkxGxyh2lcxkVhgz9c8mFmZiV8maMZPRMYpdJTOZFcZM/bOJhZnYVbJmzGR0jGJXyUxmhTGvX//u2YWxa5PaXrJmzKQ7Y6ntJTOZFca8Qv3ua00ZxrUTcng+e1jJmjGTE+xhJTOZFcZM/ZXkCAfuMSVrxkzKEQ7cY0pmMiuMeYX6kVKyZsxkiZKZzApjpv4HKlkzZrJEyUxmhTHX1/+OAtk1YyYLZWcyK4y5sn7vjYlJcu588mikybmbKYy5sn7g5YQxUz+0CGOmfmgRxkz90CKMmfqhRRgz9UOLMOZ8/cBu+PHn6gd2jPqhF/VDL+qHXtQPvagfelE/9KJ+6EX90Iv6oRf1Qy/qh17UD72oH3pRP/SifuhF/dCL+qEX9UMv6ode1A+9qB96UT/0on7oRf3Qi/qhF/VDL+qHXtQPvagfelE/9KJ+6EX90Iv6oRf1Qy/qh17UD72oH3pRP/SifuhF/dBrdv3/KSOf1jD50hPk0/D6auq/5rxWK/sbEQpR/w5HhEIPr/9yOkR8+PjVOWe9/uzOyeTjEi2N6OvHD/JUZkjOBqzm4fWPbiu7TiK+ro7TxX44/1M0NqLuDTCetXt0Hx1W9/r12/79bMq1N6LbgEzxtUNCmSfWb9fYPDD7zH/tDUB/TP/oYK/woeGAulBaHJE50+3XuiGhTIP1n07jBc+5j5+8DvY5JVua1uaIzFcXlUNCmfbq95bcu/oNx4RMKumUJjU5ovDkWF979eePke59icyKNTiijjw51tdI/faKOL8V5/6hrv/mRmTIk2N9z63fPO5v2itb8e+dq27/GxvRQJ4c66upv4R82t/IcnaX6uFbH3X3Cf0ZvNr77uRh0+RLT5BP+/uQEY2Ck2N1s+sHdoP6oRf1Qy/qh17UD72oH3pRP/SifuhF/dBrdv3yj0AT5NOA9tTUb370ZQL14yVQP/R6eP3mxx3n/+TlI3g/YVZn+Lm027CWnsq3+gmR9/D6/z51Yf1PvWL9K3jitMCg/nmof082r9/eCX20fxOljzJyhzRsGreZ9tyf4R+OiTRpf9jeniL3iSInMYZTnS7BnU/pSxpP4T4aN7mTFBt4/JVjkY3r7xaw+7j7r62jT8FG5f71qKGLMRrv/4/gHx1r121qaM6eVD41cZJxs/9Cy19SP4L+Y2cs4rXZR4mBR6YIS21cv7336NbQ2Tgu5/3i2i23bcMe7G31zmGf6PML855+Pz57Eu8A+1LtiQtekr/x9vjr8J5wtg6PkgMPpgiLbVz/cOHyLmD2LWEeDPU7Szw88tfdXModbmJGUH/sE02fROYY1J99Sf5YrGT90YGHrxyLbVz//T7BvymOLa1sbrgEytRkq0JZ/ZMn8a7cts2J+oOzeWcYHyTrjxwce+VYbNv6zaVR5hFd2m7hx4/uB4h1H49xD3J9tX81vLvdyHyixEnEK5msP342Z2M3A+N+57XZEyYGHnnlWKqm/hL2+H4Jh+T7lf1w/4aGfR+klta8V9y3S7ju9piwWmM4oHti9hPlT/JxfM9E63cO9s42ubE7gz2he7C7JfrKscjs+pcYr3bhI2Bzm9bvXgLvlzzgeTauH2gI9UMv6ode1A+9qB96UT/0on7oRf3Qi/qhF/VDL+qHXtQPvagfelE/9KJ+6EX90Iv6oRf1Qy/qh17UD72oH3pRP/SifuhF/dCL+qEX9UMv6ode1A+9qB96UT/0itb/fyzfWnH4LMzjAAAAAElFTkSuQmCC>

[image3]: <data:image/png;base64,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>

[image4]: <data:image/png;base64,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>

[image5]: <data:image/png;base64,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>

[image6]: <data:image/png;base64,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>

[image7]: <data:image/png;base64,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>

[image8]: <data:image/png;base64,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>

[image9]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAP0AAAA1CAIAAAAF7HpyAAADF0lEQVR4Xu2b25EiMQxFCYdwOo3NgFxg2Q5n8uAHimcGLLRpW5YlM5hubNr3fMGVLKaGY+OaGmYXACbN+Xw+nU7H4/FwOOz3+91ut91uZ7wLgGkB70GNwHtQI/Ae1Ai8BzUC70GNwHtQI7L3LQCT5l/Her3+27FarZbL5Yx3ATAt4D2oEXifwrWDp+B7KNH7NKu0VVpuiFc10lYl8LEXqg14H6tqpK1K4GMvVBslej8sY6gzxkyRj71QbcD7FMaYKfKxF6qNEr0P32yTUGiV9oQJhVZpjxhaWNU28FTHn8fXijkNWQm8z5d5rzVoYSQ3hFWWhA1aqBE200R7rCVgEL7J+1eTeG6IV1ulQQw1Is1hiSVhAxgEeC9XTW4RqyyMEJ8TwhrICjAM8F6o0vBpw+8xq+jap3OeNoA04D2vsiRs0MLfY5c/nfO0AaQB73mVJuZxuFwMNcJmmrBq/CkYiul73+r62pIYMsIGmsTxJ/G1kRKtshy8Q4neAzA28B7UCLwHNQLvQY3I3vNv4QIwLeTvlfMuAKYFvAc1Au9BjcB7UCPwHtTIxL3nf74qhuv1yn9W8EGm7/2f8thsNvA+L1V4b/+1qxDgfXbgfQbgfXbg/VjMengB3hdAkvc/i7l9V280rcvnix+vlXNrse3DoU7N5T399fAavC+ABO/v1ju9uz3wkA7e98D7wknwvm3IEe9tA3hPgPclk+B9J76nfg/x/t4j7QHVUHt5cnuIDqBPWadJ5KnwHoikeH9x6vn2996zTwSKYqjT2j26TbFm31OzTurUpsJ7oJDo/QNz8tODeL5YKCe9QTbUaX2nbbj4vvZhpzz1Au+Bwnved7jT3XwKzJuGuukjG/rYPw5f8m4/mZ0kd8pTL/AeKLzuPble8MD66Z/KFNlQ7WJk5tALj9wpT73Ae6DwuvfmVPdu3r2J/rksXnYUQ8lmogPNE+/eJHYqU+E9UEjw/g69bjgpifeRk9mt7Oh73EhvlTAm7IT34DUSvf8WcnlvpRfVh/fZgfcZgPfZgfcZgPfZmb73myKB93kRvf8PoavyBMds+1EAAAAASUVORK5CYII=>

[image10]: <data:image/png;base64,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>

[image11]: <data:image/png;base64,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>

[image12]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAQEAAAAZCAIAAAC+fAzoAAABBklEQVR4Xu3bWw6CMBCFYfe/BRBdHoSbO9CExEkzLQVkYkjn/55k5rTpy3n09v56AaWb53mapnEch2Ho+77rurZtb3QAftABeLfdgSdQtMeiaZr7oq7rqqroAByhA/Duoh2QByg6t+W3U3Dl0h3Q0+Os7kHB6AC8s+mA5GM6us+ZsyGre1AwOgDvLDugpydkLpS3iaMBIGTZgZBOHKSvW6hV+KkOJpNA0qU7oKeLeCWTzApYY9kBPT0hc2G8kklmBayhA/DOsgNJOrpP5my8kklmBawpqgPJ3yoMKDYdMCcPUGQVh9VnKMgC2kU7APwNHYB3dADe0QF4t90BoGzb/6kHypbswAce3eD6Wv35xAAAAABJRU5ErkJggg==>

[image13]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAPoAAAECCAIAAABYOF2qAAAWs0lEQVR4Xu2dPW8UyRaG/WsW5xvgH4AgIJkA9gfgaAMCJJBJSCDDIl1ptUKafEE3WAkLbOlqJUv2SpsSXNhk5QgEu46RwLc+uqrOOVXVXT3u6e7pfp/A7jn10TOep86cGXfZW+cATJp///33n3/++fz586dPn7ZkIwDTArqDGQHdwYwYu+4vwaSRz/ea2QDd/wYTBbpLoPuEge4S6D5hoLsEuk+Yket++nB7a2v74SkLLhdbW1uLJYt1B9X98N53W47v7h2Sn1uKn27e/EnGxsFPN7fGetf6ZeS6W+G579r2tcmeyO6H9242mm4Yre561Tav1jkwet0j39ds+3h0Lz5vIyq5byG9a8avuxE8+M5tN8WOhnbQx8kqqIh63Y05vrJxxY5VqdJdd4ndCvXETzftaDnVzXs/6enioZdF3cubN6uTTpjffvtNhqLgJuhOfc+oTxaB0n2xkAVQC+p01+JoG6uQu1kJbHTPlslumqqzmMqunPTIS6Jm/k6vpEn7/vbt2x9++OGXX36hQXVTBVWTj2yE7sFmZvvpqTdad6gaLlvs1Op+6Mys/GaGKt3v1VhlPfea86l8vHvsAswuw8kgjI9d/3tTdHcOM9ursMfrvnJiN9TpbgsQnSt9iNTFxv6bWa2M0MFqMVV3lbogfLqUXYiTwRufdP3vjdHdpu9tVqOw97Dqxvp1N2UBcdO9N60yp72ZT9O0hpZTJXSPIyvh3irk79eksMYnXf97c3Sv3pRSk4nuJsv3p7v7Ht50htq9ziud0KsUK6fqSu4I+nIy/fRueGuQUcPG6G7sFjU5+VzmoS/ehe6qT7tKvkb3qgLZUkm6krT6dMUleftdlw9p4WkBLaZak+5+ObHD2bI5uvdFpDuYDtBdAt0nDHSXQPcJA90l0H3CQHcJdJ8w0F3yEkwa+XyvmbHrDkCHQHcwI6A7mBHQHcwI6A5mBHQHM6JY9+VisTw/2VvsnciWAlqNa9UZgBaU6m4dPNnba3U9o6OVwbWdl4sr+UYA6inXXYtuUvwK1Bosqe0M3cElKNJ974q9kN2QEr7atudMFDeFwbT1ZO+K38PtJs52JpsDq6mjEwFQR5HuiuWeqWWSedc5q+TT391N0psd0s4NusdT0ewuzgtAE0z3vyJct7rSPShb3T7RPqpoyLpBV9G5SfdoKqK7H6sOfFA+AAAI79+/f/fu3f8MNdm9rnSXutsS48reMpXd2+keT8V1l8UNALUUFTP1tTtVVmnnc22ymIk7+wwd656YihYzeNsKWlKk+3lt6e5V9eW4sZCWGIlyvCq4q6SuM3VedzKVqN3NDTUVvAcllOleV7prqrrC521TXywWXlK2UJKd9/byxQybin0Yw6cCoIE2uqdLdwA2hjLdAZgE0H0wjo+P79+/f+PGjZ2dHfVVHauI7AQ6BboPgPpxP378eHd39+Dg4MOHD1++fFFf1bGKqLh6JuQA0BHQfQAePXr09OnTb9++XXBUZH9/X7XKAaAjoHvfqIrlzp071u83b97cvn1bFTPqqzq2QdUqqhr9aZT80+ItPjXwf7zT0Pgnalv/Hc+WQ1p17hjo3jcPHjx49eqV0vro6Oh7jjVetao6ng6xH74SR9rozteKmap+7Ao6NgxpaO4R6N43165dUz9upbXK6EJ3FVHxjx8/Xr9+nQ5Rjuq/ph80Lded/dl9gxa+dvAKcjYMaWjuEejeN0prW7SoGkboriIqrt65qgM6xNhNxaW6+1IlZVRsuw37fyxE/ko5CYWpqt/qZVaa+3PmZIic0E0QZqHzx3feP1ZNFesO6N43q2V3rUMoS7xzxgsnUcL4jO6OkOlJzvc6minNaDJ5ve7JCUV25/PLO2+Wh5vVj+kK6N43qi4/ODhQWh8eHgrdX79+reKqNa7duRUuQJ0yrcQqH8vrnv7vcU5HOZ/tXat7esKM7uk7vxT/8KtboHvfHB8f7+7ufv36VZmt/L5169bVq1fVV+u6iqvW+JOZyotKeKI7cSNlCnPKQDJplUsdQvfUdA26pyfM656482z+zoHuA/DkyZP9/f3c5+6qVfSnCjifahIkI/I9BFjqP/X/PS6Z3Wn2pfeF6Z6ZMK97w/zdA90HQP2s/W9VVaWu3puqrzW/VWUKaDGc7kQwE06JYvtXLeYGsdAdm0UkdJft9pAq7sfEutMJifm62knMT+48dJ8o9poZ9a50Z2dHfa25ZkYowM126b7OEt9H/JapWjo6Gv57nNeRDYxDC/2fdcWQ5IRkkA4k56eRmgdyWaA7mBHQHcwI6A5mBHQHM6JU9yUo4+LiQv7swGhoofuPoImzszPoPmba6S6jgAPdRw5075JWumOvav90oLutWWV0coTynEP7FOr+GXtVBwK6F8EEj/DdCnXHXtWhgO7NJM1Oxkt0p3tVk8R7Vf2v5j3sUoAi6K/u58squtOnOQcfvdnUPCjRVKK736uaI96rKlUNF2yVI+eYJ9C9mZoHJZpKdPe7mXLEu5kiVbXvLeWN5pglq+guyKkwGYTTNU0lun/v9qrmiPeqSlXDlbPVDVbfnOrLbfX1iKzoIXOQyxbpK4S7OpGeKpp8w4HuzQina5pKdF8tu1sTA5WTQXx+VHUgVY/XPbwykNcIM8b0NEc2mpp8w4HuzQina5pKdPd7VXPEe1Vpdl/SLW9aw5CMq6vPTXYnqd8ba3omt5PyeapOyck3HOjejHC6pqlEd7pXNSa5V5Xqziwk2zZC0qe6h9UR5mCDbCNbQ47k5BtOB7pPHu+01zoXL9H9vP1eVaY7rSySb1lrdWdlie/JErm7kZx8w1lFd/o05+CjNx758Di+W6Hun1ruVeW6UxGJvfrQZXdapIRmobvJ3mQwCadGT8J96F6KfIQO2qdQd0v5XlWpO7Mv1BxVQOfsxaJ6c0tzdlghhsx2UqZ1MrjBrKI7yNFK93WhdY8KcWCA7l0C3UdOC93PQAHD6w7ylOrOPkEAtcifHRgNpboDMAGgO5gR0B3MiG50/7MMOWzeYK9q/3Smu3y/FgHdPZ+xV3UgoPsAYK/qUAym+/HPd39mL91RoA1qcIIn/3mXnvbdf57EwZ5YYa9qhfmNfs0v85PX6CaDs2UiugeUydpyT0fTdscKe1UN7lqXvO9Js5PB2QLd+2aF3Uwae3mivqgr63vS7GRwtoxSd1+asB5KZF+i5Enq7oa6+Ugxkz7XGllhr+p5uBhXf+e+u4sWF0tidjIIxqi7OqiEDUfGdXscjpIkdHcuszlqzrVeVsru4dJz7nuIk6vXk0GgGZ/uXOd378yRDobuxz/nzUzp7ke6gU735LnWzAp7VZnjQWbhvtu4lAwCw/h0t4cWr6KPOLKlR0L3eJ2kipn8CuqWFfaqhk0Wjkphtr/OXfibDALDaHTn+dsSMjPL0bW01T00lZ7h8rTcq8r8Pc9WNsjuzQymu9abVeYJs0mtIQ7zZrbSPX2utfOp1V5Vabv0PRzVBYGmM91LEKNohUJNs/IbqNWhe9b185a6Z8/VB4V7VbWx0WePZAm4Skd/SunNTgZBR7oDsBFAdzAjoDuYEdAdzIix6/5fMGnk871mNkB3+tkOmBLQXQLdJwx0l0D3CQPdJdB9wkB3CdX96Pkj9xvQu4+eH4UfW5IXz569kLFx8OLZ3bHetX6B7pIoux89f9ZoumG0uutV27xa5wB0l4xG9+LzNqKS+12kdw10l9TrbszxlY0rdqxKle66S+xWqCdePLOj5VTPnr/Q08VDL4u6l8+eVSedML///rsMRUHoLqnTXYvzIoTczUpgo3u2THbTVJ3FVHblpEdeEjXzI72SJu37H3/88euvvx4eHtKguqmCqslHoLukVvcjZ2blNzNU6f68xirrudecT+Xj3WMXYHYZTgZhfOz6n9A9pk53W4DoXOlDpC429j/LamWEDlaLqbqr1AXh06XsQpwM3vik639C95ga3U1ZQNx0702rzGlv5tM0raHlVAnd48hKuLcK+fs1KazxSdf/hO4xBbq77+FNZ6jd67zSCb1KsXKqruSOoC8n00/vhj8MMmqA7pIa3asK5K5K0pWk1acrLsnb77p8SAtPC2gx1Zp098uJHc4W6C6JdAfTAbpLoPuEge4S6D5hoLsEuk8Y6C6B7hMGukv+CyaNfL7XzNh1B6BDoDuYEdAdzAjoDmYEdAczArqDGQHdwYxguv8FwKR5//79u3fv/mdAdgcTB8UMmBHQHcwI6A5mBHQHMwK6gxkB3cGMgO5gRkB3MCOgO5gR0B3MCOgOZkSx7svFluLK3olsyLNcLJYyVs3TdqoUJ3tXEvMDkKdQdyWpVqudYRndbbDdVCkuPwOYG2W6LxdVLq60L6NW9zDnqkB30JYi3SOxVMAUJDaqDN6zAW+yqVaupHR0uruFc7K32Fvq0WxsVepkZrat+l7xVgDqKdJd5nSXmKu4+ubttDdNbznK4mp3l9rNyvH93MLSi+Akmtm12pn1SO99fCIAIpjucu/HX3/ZTkLcoJf1npQ66sC3pi2MszvtdXKiJ/IiZ2au+pK74ePyAQBAKNrNRMVSkgb7ne70Jm2t0d1NWuXx0Gwy/9Jldzpzie4A1FBUzGSzbEr3wuye1F3F7Il8MZOcOb6ZOBEAEWW666SrjeLfwk1hv4nqiiRhYVzMJHR338XMTnBfu0N30IpC3Y1i5CMQe8unfCZl1Xplby9loR0ZpkoVM1tXFgvjezSzXkNuLHQHbSnWHYDNB7qDGQHdB+P4+Pj+/fs3btzY2dlRX9WxishOoFOg+wCoH/fjx493d3cPDg4+fPjw5csX9VUdq4iKq2dCDgAdAd0H4NGjR0+fPv327dsFR0X29/dVqxwAOgK6942qWO7cuWP9fvPmze3bt1Uxo76qYxtUraKq0R9XbT88ZYEWn0WdPtyuPg3TsIlWY7lITOI/cnO0uIdtaPfYBdC9bx48ePDq1Sul9dHR0fcca7xqVXU8HcI/BLaB4qecrxUzVfHYDBnd5Yq89HmStHnsEdC9b65du6Z+3EprldGF7iqi4h8/frx+/Todop7h7e1tok/5U64zO3ezAw9LdI9P3BHljz0BdO8bpbUtWlQNI3RXERVX71zVAR1inmHqD33KfamSkiAj3empD8XD/dnil5QqVKA7v7f6yMxoQ/6c7FUneVJy1qaeJUD3vlktu+vnNZQlXnfztJvDcETJ6O5IDjeCBS/lEWlmOC8rSA+l+2LB1io5U3WXsye1h/LuhTu9bYaXAt37RtXlBwcHSuvDw0Oh++vXr1Vctca1O3fSBYgvtlX6Xq97eriK0qzrdfM9WQdPnN2pyuGOkVeWsH6bT3ruRrLZVM/QXgB075vj4+Pd3d2vX78qs5Xft27dunr1qvpqXVdx1Rp/MlM9wZVFRHeWRWMNuWoanigTw8PZTNDF2MKIziN1p0NSLfJVIHPS5Hl4T9rWCHQfgCdPnuzv7+c+d1etoj99hp0rTndmoTBbE/lOAunh0qco0eY0jKVO6M5eb8LCaTxp9u65wyKg+wCon7X/raqq1NV7U/W15req7Bk22dmZFOSh1QPD9q9azI0gX3K49Mm3k55NurOTRi1B8drsLvvaQ9nTHRYB3QfDXjOj3pXu7OyorzXXzLBnWJptpAkLIInvE4saD5c+uQGu5/bDhyKRs+YK0kPk/Wq92olyEsuThvsne7rDIqA7mBHQHcwI6A5mBHQHMwK6gxkB3cGMgO5gRkD3wcBe1f6B7gPwGXtVBwK6DwD2qg4FdO8bulc1idir6n7nnrxGQDUm442I6xLmAnTvG79XNUe8V5VKzQU/fbiIr15JwwdCd+jeC343U454N1NW9xa2Q3cNdO+b791e1RzxXlVnKrvoUNoaX9qYaPPtRvdEneRCiUseDcmzkGsc3Sg9/0MbNsFqHLtvTedaA6W6L0EZylf5s+N0md0DS7/lIhxxouzu7NPWyevLwxEjeRY9lZ05HNn59XHltHfe3YfGc62FFrr/CJo4Ozu7aNLd71XNEe9VbdadK0O2gwZi3f3N1O4hF6Qkz5Lde0rEJodVe+O51kM73WUUcEp0p3tVY5J7VZt1P3f5Ol8bRLpHtvkZHIkTpc7CxhXqXnKuNQDdu6RE9/P2e1WLdHewvE0o0z01MoXvyzI+07lR99JzdUgHutuaVUYnRyjPObRPoe6fWu5VFbq7eoCULMQe5h+BD0zpTofqw8jH1FnIGJOxC3VvPNd6gO5FMMEjfLdC3S2Fe1WNDPTV3tUB3GnXraYuoAOTupM+mVlSZ/GxzN7TtO62j5xr3UD3ZpJmJ+OtdAf9s4ru9GnOwUdvNjUPSjRB95ED3ZupeVCiCbqPnFV0F+RUmAzC6Zom6D5yoHszwumaJug+cqB7M8LpmiboPnKgezPC6Zom6D5yOtB98ninvda5eCvdsVe1f1bRnT7NOfjojUc+PI7vVqj7Z+xVHQjoXop8hA7ap1B37FUdilV0BzlKdB94r6r5zT0bw3+zP22ge5eU6D7UXlVDau1A9xil+xko4KJJ9y53M61iu72Qiy0Z6C6RzwnII392nCH2qlZY2+2/OKVrZtsuATnYvRRUQTeatNKreTWjXzaluoOu6DK7B5apXaSCICjz3bjqL9uNL0IPR3TqMAWfNnPukQDd+2aovarMcZGaw1BntBafLQjhO7c93KPedp2uBnTvm6H2qrJKiHbM6U7m8bec22RMNG/67o0D6D4AQ+xVleGQ33O6J7K7O6SLQU48aqD7AHzqf69qLKXvpw+qEbpXyPkko3P1wysD7ym6jhDoPhg97lVNa1gtAb2AFgv2IUxoj4KplUMKmugk4wK6gxkB3cGMgO5gRkB3MCOgO5gR0B3MCOgOZgR0HwzsVe0f6D4An7FXdSCg+wBgr+pQQPe+GWqvassrF1vMvEFA974Zaq+quBI9ceULA7qDLuhyN1ML26Xu5DrGJNAddMFQe1W57sJ2VzGFkFhidGoylk5DL4IfK9C9b7rM7oHgWs46UbsnL1jnIge5ref+SB/YRjOpHZE776iA7n0z1F7VOLuTZB2mJP+WzL+isDRPfV/a/41dhcZvO3TvneH2qvIG73H6IxuiOxnnblm5te1L47z5f/Pp+zUqoPsADLFXtV73eEBtdrfeLxY2oWvfF/Q/+Y0X6D4An/rfqyp0151SI0ycZXfSGhrPq9eEapQ53gTboftw9LhXVSNqljjVW9xgusR8K53Z1TW2b3KFjQ/oDmYEdAczArqDGQHdwYxI6y7+HwsA5VxcXMjQaPj27Vta9x8BaI/9hw7j9Efdtzrd/U0ACqG6y7ahge6gY6A7mBFD6R7Kcw7tM3bd3x4uly9Pz1jg8G24CUbHILpLxzm+2ybovqSCQ/ex07/uSbOT8Q3Q/eXLl0R46D52BtRdNkRNG6D74duz05e+oqG667iBLwYXJsvChVhdBNbCButuawnG4dvegufe7lDCe92NweYwHNnax3YMayRxlDxdb0H3M54mG6y7vzkUzm6vtAtoj1iad9GQwN8emmPfSINgbUD31fHJ3AlPdCfeuluhuwmmM/nU0+vAQPfVof46b+uze1J3GN4f0H11mL/VG04b4BV5FUzpTnrSrmBNDKi71zoX3yjdpa6+TKGRWHfaE66vnf51P+dmx/huY9cdbByD6H6eN572ge6gY4bSvQToDjpmU3U/A2AlrO4yOg7Sul8AMEXSugMwSdJbswGYJNAdzAjoDmYEdAczArqDGQHdwYyA7mBGUN3/D8tcHAY3+flGAAAAAElFTkSuQmCC>

[image14]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAP4AAABZCAIAAACR7Q5QAAAFCUlEQVR4Xu3bO3LiSACAYU7DBByFhMQ+w25I5HROQeKyl5iqjRz6AL7AmGJgq2zsyOXXxH5ppX6puyWDECAh9f8lRt0NM1X+1bQZTyeKoj9A272+vr68vDw/Pz89PT0+Pj48PHRIHyEgfQSK9BGosumPBx1hMDZDVyeDkytrCXDIyqefRH918sOOH2iO7dIXuz/to4m2TT/e93+oY0564Envh/FAzqrzkVqarBzHbxhqVfLeoYgBZ3H8B53IefOS9kv5l0BB26ZvZW6d9fVDVX7ctm5afBWtmzcLNavvEm9x/MXcA+PMrP/KQFGr0v8vj3re6vRVxypM3ad5izAD9vPMF3exviPkA/eZmcWC/zcG8iwWi/l8/luYzWbT6dRJXzeWsfLAo5rUYV6lBxq5g3ufBTmfFvmLrXssN31nMVDYml3fXWzJ+THXCTppdOBs2HpGTNrpm7j1pbP4u/TlMv+VgaK2Sz/ZdU23ftDpRqzfGvS53V1ptm73fUQtdtOXbyfy5dVfwHlloKjy6WdOGdljTLqbq7zVgH+TOHF7i1fPZi6Bgsqmvzv+GQaoRP3p/zFvIezcqNBBpA9Uj/QRKNJHoEgfgSJ9BGqz9P9Fq3nf7nbbOP0lWor0ST9QpE/6gSJ90g8U6RdOf9Tvj9KrotJnXQ77w0t3EjUi/crSx2EhfdIPFOmXT3/UF7+A2RXHmHh2OOwm12qVnO12uzkHHn/xpbywno2tXFxc+EOZQdIvm36cq7hWRcelm3tgJC7FrP6aLkx4i9VLjfpyENu5vr4+Pj4+PT21B+PLeDCeMiOkXzr9yyTTZL8WwZpuxQN9X5gbRDy00rcX6xl+Dt4Zr/5s90vSL5++PNF0hyO965uDTlxzutnnfsLjLlYvxWlnp0z9ud0vSb90+vF2Lnduc+Cxa1676zvp+zcVdkPWn9v9kvS3Tl9/zdQstvDkPLQ+ffNTLnfArl0L/qhA+hukrz+F0ad78bDfF+37Zxg53R0OCxx4Ms9FBUi/cPp7kx6KSL9CpF9/+kvzjsKBp0KkfxDpo3qkT/qBIv016aPFvG93u22WPtAapI9AkT4CRfoI1Gbpj4Gy4q78oVr9I5yfn5+dnb29va1P/29gc/f39zJ9f6JWfwk3NzdF04+ADdnp+3M1+fr6+vz8/Pj4IH3sEekjUHWlb3652BM1NP3JUafT+/nLGTiapJc4OLWk7/fuamz6HTt20j901advV/7deCPT7/V6Vvykf+hqTN+fsKYamf7R5NfPnjn12Okn44J7Y+hh6xbRQ87ZCXvRkvTlecNxNKlsMDKlp0d+k76oWTxMH8nzkVyY3i85j3L/uMoG1belpVqSvv9KldOlm7z1QNKUs/3r0XRjnxyJx2bSHsTekP5umE1ex2+lbzWsr9LlYjB/h2/7tlsz0t8Nu2Xd8OpdPzd9aq8O6e+G07L6YVUOuCd4NZiXvrXSXoo9qTH9Tss+3LRLddM1Rxl7JJu+vZLu96769CO38qxG/pMWGqeW9KPv648a+osMaJy60l+B9FGFNqR/D5QSifT90frc3d0tl8vb29tC6Xv3DdBcZtd/f39fnz7QGpv9t3SgNUgfgSJ9BIr0Eag16QNttVgs5vP5b2E2m02n0zR9oMVW7fpAi5E+AkX6CBTpI1Ckj0CRPgJF+ggU6SNQpI9AkT4CRfoIFOkjUKSPQJE+AkX6CBTpI1Ckj0Bl0/8fi7twEPkVfGAAAAAASUVORK5CYII=>

[image15]: <data:image/png;base64,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>

[image16]: <data:image/png;base64,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>

[image17]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAP0AAADVCAIAAAATqGHwAAAPnElEQVR4Xu2cTZLbRhKF+zZyhC7TG93BfYLW0tewwyNzrYvIeyscHs8s7OkIhx3+Ge5tD4esIhJZWZmJQqFAAMT7FlTx5Ss0u/ERxHhaejidTkcA7pH/Bv7444/ff//9t99++/XXX3/55Zeff/75p59+eoD34F6B92CPwHuwR+A92CPD3h8AuDu+6nj37t0/Al9++eUXX3zx+eefw3twt8B7sEdW7f2JIWfro+R18u+opA9mYr3ecy02oUjJixQddYsagrZM9T6epHie1DUlo8h35cnaKHmFeackAc1p4L1YW4+jyM99nqyNkleYd/IE3IDNeL9+Sl5z3skTcAO26n0siBoPicFRPrVyPiosCPJOkyQ+zWtiJApyYIzUUIzUgjXi+VI08J6+GbEWj6Og46jwab4m+DSdKKN4BOuA+chvWuSd6Ym15okIKc/Xziiu1ZH/1FovyFTvZ8L56eQjSuoWAp7nHWd7nuTknZJEhNZaTfJCnltrkYwaEfkoT24PvE8OQvCQOjxxRg55pyQRYb7OUTcKnH4Ojfr97k+DiKMc2bstm/GekpLRqEXhWiTOyCHvlCQ8F1OrTFiFsflBG1GSjwhntCDwXh6QPxUjnjgjh7xTkvBcTK0yYRX6Y3XwPO1eyUeU5CPCGS3ISr0/pD8v8bOzRrSuW9BaPI3rPMnXopwjOuoWNYyoI/+YJVtEYh2hbpc/Wor1en/ofkDqj0kd0dNRC3E0QnR4UzzlsK6CbGt9Kz/Yo/5wWSFP1FAk8SmhdvKk35Dm/mgRVu09mA8h4mkFLt4SeA/2CLwHewTegz0y7D0A98fw3ysH4P6A92CPwHuwR+A92CPwfgPI/xixGrZrCLzfAGfDntbHy8vLdg2B9xsgeh9+mWBFwHswL/C+OfB+A9ze+wcD3tmd91+//YT9KD55+7UsrITDo3xth0f2wolL6/w9PR6S7oq4sffy55NCtZ15f3Gn9ymYtFJjcu97zp7bw7VxS+9VxdV8V95frvWpMBfz1yk+vK9AyO2M9uR9rn2Mv6aIboLovXB+Yzy+jWnYer3Z6N8rgwV21OST5vHQ5azb3cs8HsZ4T/c5BS+GXo19+LbA++a08b4jCBEU6Vf9rdBVF9Kpc6mo0B/rmoZd8bX0r6pfsbGG573/YrQvNjPwvjlNved6hGp4Ei7MlLFld6ChAvs0uXyFa3pe9q/kemmnr5kVJJ73/ovhX6L7ujMD75sz0vtU7UC4LpIzTIPu2ZBJw4V4Ee7ovc/8k288W0o5LPY+eSkX0p/GLMD75oz1PhefBVK7+GTIpKHC5UDkaCJg5j19zWthJu9voHoCvG/OaO+DW3Tqw5NeoF7RMCgzaajAvA8XW8d7+QJm8J6/HPZNzsoi3pPfVr4z7y+wD3upFo34ddc3abAQ/Ipf7C3d4Kvex5y68sX11HsfO/KbnJVben9KFc+h2g69Bzflxt6fbPV5B96Debm99yXAezAv8L458H4DnL1/WSXbNQTeb4D0Orsu5GvdCPAe7BF4D/YIvAd7BN6DPQLvwR6B92CPwHuwR+A92CPTvI+/mPjq+YMcNOLweJtfeAR7Y4r311/Z/fD8ai474T2YhwneHx6vF/rkN+GbAu/BPNR7n13mz0H4He3rZ8CZ8/rxcif0+Jw+vWy7/t2N+NY5+/0cd18Peb2BejXbJwnYN/Xey6t8d/kP+cX75w+xEqXmTw/0pvnw/HjZdJ7QG+DQH1p+CQAaMeD9vzXiTiFlf/m/vAEO4Vl8K0Tv+dOz9x8ull8+IYLv7JbpvKBDZR8p94z8KYM5+Vfg+++//2fgu++++/bbbz9+/PjNN99cvZfnp4OL/nD5x8n6e5Rh7y97Ljc5h+56z/ZePxfi0/14D27J8PVe7iCMi3SJ9/E26Mjuc7j3+7zeg1sywfvLNft66WZ/xD+jsYPed3+m3neHutwGwXswB1O8v96tkJvxWSe05z11Hx+D+ML76/jV8zPuc8AsTPMegG0C78Eegfdgj8B7sEdGeH8AoJazSzJalK863r179+effw54/wTAeOK/MLVCfz799NMffvihyPsTACPh3svZQvwv8Pfff8N7MBfwHuyRpbwPv+mucNqi9+/fPDy8/uxjErx53z8Fq2MR76XsKdv0/oGbDu/Xzu2954pb+fa8f/36NTMf3q+dBb2XAzbanvdv3n/87DXd7HDvL3kgfVd0MXt/dFFyywRm4R68j7cZCW/e3yw8keb9bT55H1QOy34Vb4tisX+zKCv1y90svJ6TO+UevJeHuTmd5uR2F1yESi78Xdpf0t+/CWsa8hDMBrxvAF3eO/OZ90zg7llfD6F+bb/3C+7CwPsGcJE7gf3rveo9VL8d8L4BicjX/3Uag/Su/Rpq3rMmr4KZWND7h3v675hc09RbuoPhSe49b0L62bm996dU8Zzt/f9WYHMs4v3JVv+0xd9TAJtjKe8d4D2Ync17/wJAFafgvUyX4z+BH3/8cdh78Y4BYLvQ9f6vv/4a8B6Au2HE3ysH4G6A92CPDHsPwP0x8O/fy7cJAHfBwPVe1gG4C+A92CPwHuwReA/2CLwHewTegz0C78Eegfdgj8B7sEfgPdgjk7w/gHUweKaAYKr3T2BpXsLfZpLnBrg08P4EFgXeV9DMe/lvNWjwswVaAe8raON9odOFNTAKeF/BVr0v+QDpP2hSZC9DbjC2yFJAlhiy2iF7HbLH4DV4X0F77+UpSke0ngIdxz+gNbXyiDqdGEackUphH95XMIv3tBZwXyOFawGF6pRwptbIylWs8tjcorAP7ytYzHsR8se8wxG5VTuNH6mhg9NXR2roUNiH9xXM4j2H8jjiHWsdn/I1RyR5gbBGY3MLp6+O1NChsA/vK5jFe1oLaJT3+WPeqQhPIbeQ1YCVWzh9dSReg9rhyLbRh/cVrM57OsF8zcsqvENY+ckYqaGD01dHauhQ2If3FczivYCPaF2Bs10dqSGRT/PEx+mrIzV0KOzD+wrae+9QWLNwtqsjNSTUqRpaWOWxuUVhH95XsBnvB/fmhTwhxo4mhhFnpFLYh/cV3Ln3FqIpkG2jL0sBWWLIKkNWA1YugPcVtPH+5J5Ugp8t0Ap4X0Ez78FSwPsKpnr/AlbA4JkCgknep9cdsCTy3ACXSd4fwDoYPFNAMNX7J7A0uM+poIH3MgW3Bd5X0Mx78cmrku4GbYD3FbTxvtDpwhoYBbyvYKvel3yAdB8zEtnLkBuMLbIUkCWGrHbIXofsMXgN3lfQ3nt5itIRradAx/EPaE2tPKJOJ4YRZ6RS2If3Fczifd9I4b5GCtcCCtUp4UytkZWrWOWxuUVhH95XsJj3IuSPeYcjcqt2HD9SQwenr47U0KGwD+8rmMV7Tt9mNT7K1/EpX3NEkhcIazQ2t3D66kgNHQr78L6CWbzvGyk0yvv8Me9UhMeQW8hqwMotnL46Eq9B7XBk2+jD+wpW5z2dYL7mZRXeIaz8aIzU0MHpqyM1dCjsw/sKZvFewEe0rsDZro7UkMineeLj9NWRGjoU9uF9Be29dyisWTjb1ZEaEupUDS2s8tjcorAP7yvYjPeDe/NCnhBjRxPDiDNSKezD+wru3HsL0RTIttGXpYAsMWSVIasBKxfA+wraeB/Xg6S7QRvgfQXNvAdLAe8rmOr9C1gBg2cKCCZ5fwKrQZ4b4DLJe3n/DhZi8EwBwVTvn8DS4D6nggbeyxTcFnhfQTPvxSevSrobtAHeV9DG+0KnC2tgFPC+gq16X/IB0n3MSGQvQ24wtshSQJYYstohex2yx+A1eF9Be+/lKUpHtJ4CHcc/oDW18og6nRhGnJFKYR/eVzCL930jhfsaKVwLKFSnhDO1RlauYpXH5haFfXhfwWLei5A/5h2OyK3acfxIDR2cvjpSQ4fCPryvYBbvOX2b1fgoX8enfM0RSV4grNHY3MLpqyM1dCjsw/sKZvG+b6TQKO/zx7xTER5DbiGrASu3cPrqSLwGtcORbaMP7ytYnfd0gvmal1V4h7DyozFSQwenr47U0KGwD+8rmMV7AR/RugJnuzpSQyKf5omP01dHauhQ2If3FbT33qGwZuFsV0dqSKhTNbSwymNzi8I+vK9gM94P7s0LeUKMHU0MI85IpbAP7yu4c+8tRFMg20ZflgKyxJBVhqwGrFwA7yto431cD5LuBm2A9xU08x4sBbyvYKr3L2AFDJ4pIJjk/QmsBnlugMsk7+X9O1iIwTMFBFO9fwJLg/ucChp4L1NwW+B9Bc28F5+8Kulu0AZ4X0Eb7wudLqyBUcD7CrbqfckHSPcxI5G9DLnB2CJLAVliyGqH7HXIHoPX4H0F7b2Xpygd0XoKdBz/gNbUyiPqdGIYcUYqhX14X8Es3veNFO5rpHAtoFCdEs7UGlm5ilUem1sU9uF9BYt5L0L+mHc4Irdqx/EjNXRw+upIDR0K+/C+glm85/RtVuOjfB2f8jVHJHmBsEZjcwunr47U0KGwD+8rmMX7vpFCo7zPH/NORXgMuYWsBqzcwumrI/Ea1A5Hto0+vK9gdd7TCeZrXlbhHcLKj8ZIDR2cvjpSQ4fCPryvYBbvBXxE6wqc7epIDYl8mic+Tl8dqaFDYR/eV9Dee4fCmoWzXR2pIaFO1dDCKo/NLQr78L6CzXg/uDcv5AkxdjQxjDgjlcI+vK/gzr23EE2BbBt9WQrIEkNWGbIasHIBvK+gjfdxPUi6G7QB3lfQzHuwFPC+gqnev4AVMHimgGCS9yewGuS5AS6TvJf372AhBs8UEEz1/gksDe5zKmjgvUzBbYH3FTTzXnzyqqS7QRvgfQVtvC90urAGRgHvK9iq9yUfIN3HjET2MuQGY4ssBWSJIasdstchewxeg/cVtPdenqJ0ROsp0HH8A1pTK4+o04lhxBmpFPbhfQWzeN83UrivkcK1gEJ1SjhTa2TlKlZ5bG5R2If3FSzmvQj5Y97hiNyqHceP1NDB6asjNXQo7MP7CmbxntO3WY2P8nV8ytcckeQFwhqNzS2cvjpSQ4fCPryvYBbv+0YKjfI+f8w7FeEx5BayGrByC6evjsRrUDsc2Tb68L6C1XlPJ5iveVmFdwgrPxojNXRw+upIDR0K+/C+glm8F/ARrStwtqsjNSTyaZ74OH11pIYOhX14X0F77x0KaxbOdnWkhoQ6VUMLqzw2tyjsw/sKNuP94N68kCfE2NHEMOKMVAr78L6CO/feQjQFsm30ZSkgSwxZZchqwMoF8L6CNt7H9SDpbtAGeF9BM+/BUsD7CqZ6/wJWwOCZAoJJ3p/AapDnBrhM8h6AjQLvwR6B92CPwHuwR+A92CPwHuwRx/v/A/q5z6e3jkZiAAAAAElFTkSuQmCC>

[image18]: <data:image/png;base64,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>

[image19]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAQIAAABQCAIAAAB9HOxPAAAEh0lEQVR4Xu3cS1LjOhTG8eymB9lMJtnD7RVk2vuAS2ecjTCmaCgu3CqeRUHx0phXOrFs+UhWFNtR4sT+/ybtnCMbqut8tsKrN00ooO3e3t5eX19fXl6en5+fnp4eHx8fHh7u7+/v7u56xAAdQQyAcjEYA632O3FwcPBvYn9/f29vjxigW4gBsPUx0J+D5vZ85HrJXQcIWx0DOcFVp7nqenTZ9sagOMfFSkClxei4aDHQi+XwOS+rKp5brARUWoyOixMDvdLwVqqqfaK24unolJgx8FaKrZKWnqgXLFoWri/qoptixkDOljyuJ3yFpR9radG7AN0UJwbjQhKKB1XJE/WxqchWcfEqFXRTtBhoZrCKB1UVTwxcM1YF3bT2GOhja1E58gpOJdBasYJuihMDvdLwVqoqnmgqgdaKFXRT/BiEi5XIc51LLTo2lha9C9BNcWKwPvpzkLwtcYbVdavLzkI3bXsMgA0gBgAxAIgBMC4ZA6DdSv1KPtBuxAAgBgAxABQxABQxAFS0GIwHvZkfo0O3oQ5HP+athK/v0lfqDcb6hf5XOBwN5FXCL4FSosRgNrrzcU3/8Sg9nbPQJJcwV1xwQYMYIIIYMRgP0tu8OXCVns7sCmkciAE2IkIMsju4LIiNja7k01nsCrOmTFIag/lGKVsenntigDoixMDdC2V3dFEX0+npWuYpMUlIYmCvDM89MUAdZWPwv49u6THVN/nZAOcPh3yPlE+nr+tKLpU+BgYj+/ngDnr4pf/TBhwXFxfn5+f/Jc7Ozk5PT09OTo6Pj4+OjtwYiOmyOJOd37x9MfB1PdLNUbIbGvA0wJotehpUi4F8i+y735d9Grgh0e8NrDcf4bknBqgjQgzmY5pti9JbeLqj8b83KHQFp5u+RZY5CM89MUAdMWKggzDbvozSIZzvZaxvllnTWehadDeNSPYF0zRo+jA098QAdcSJAbDTiAFADABiAChiAChiAChiAChiAChiAKhVYuD+/TugtOmW/VV9+ccb39/fq8XgJ1Dd7e2tjoHbaNQ/icvLyzox0GuA8mQM3F5Dvr+/v76+Pj8/iQE2hBgAjcUg+dFlj2kLYjAZ9nr9X3+swnCSv8TWaSQG7uzbWhGDnhx8YrDtNh8DOfGL6jsfg36/L4JADLZdgzFwG6K18zEYTv786pudkYzBvJ6wQ5KVRVyykrW/wlq0MAZ6T2IZTjZWnJqpz98imBgkk50c5kd6D6UX5tnxHHk/3MaK+n++rVoYA/uaDcim3ox6VpjPl/VYyKr5DX8yTI5NUxaxNsQgPnPzz4IgYiDmOXuVL0+K/jt/22/HDSMG8cm5zuY5/DTwxoDJ3xxiEJ811+kbXV2wd/xp0RcDsVIuxZo0GINei79gKqfWHmOz3ZGVYgzkSjKwdpuPwdSe+KKd//YZdk4jMZguTsK0BT9MgZ3TVAwCiAE2rW0xuAVqmSYxcKvNubm5ub6+vrq6qhwDkyRg15mnwcfHR7UYAK1R/1fygdYgBgAxAIgBoIgBoIgBoIgBoIgBoIgBoIgBoIIx+AtobbGwH1tnwAAAAABJRU5ErkJggg==>

[image20]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAQAAAABLCAIAAAAQ+M4qAAAEfElEQVR4Xu3cS1LbQBAGYN+GhS/jje+QnMBb7gEhXvsivgBQBFLFawPFK2tejj0tjXoeautpWer/2yD3jAZXqn9p5ACjlfEPYOje3t5eX19fXl6en5+fnp4eHx8fHh5GCAAogQCAalsCMAcYtN/GycnJL+P4+Pjo6AgBAC0QAFBtfwNA74H4YyLhFGEIdNrTAPBOLdu1wnxhCHTaxwCEbRpWBMJkYQh0aiAANJn3lveyrPDcsCIQJgtDoFPdANBMK1opq/KJRDhdGAKdmglAXqVat4VrcjRq+cPi6cIQ6NRMAHhj8eNqhBW8oejMaJEIQ6BT3QDMgwzYg8qKrxCdGS0SYQh0aiAAxPZW/SbbugJNsKKjXpEIQ6BTuwGo1m18hbCSd2xFi0QYAp3qBoBmWtFKWeGJtuINhTPzikQYAp2aDEBYZBPL4afLx/alFS0SYQh0qhuA9tB7CFuW161wAq9YwhDotL8BANgBBABUQwBANQQAVNsSAIBh2/JL8QDDhgCAaggAqIYAgGoIAKiGAIBqtQMwn4zWDmZLf4AsZxM+5L100EqjyZxe0FdGXkpaGSBPzQCsm3bTqMvZQdCvpHCbpkskK0YC4Cm8MkC+egGYT5JLf9K2ocJtmi6VBAEBgJ2oFYDwwp9sY7IdUeE2Xa/F91FJADbrpd9CXip/ZYB8tQLgX/fTQLBmLNGm67Oz5JgAuOvLS0krA+TZHoC/MTQUBGC5aUGnkcu16eZUWnIdgJl7T/DPlV/G3zaA5+rq6vLy8o9xcXFxfn5+dnbmB4D1lSPbAtlHV7P/mVe6A5BkK2T2PhPcAaBlhe4A7ikMewheH9htfIUtUHYzoTXpGcB5yJCXyl0ZQFAvAOzKv/6SBoA/zxZu0zQB7segPAHyUvkrA+SrGYBk15Ndvc1nQJOJTUCJNk0+QaKl0o9BN08FyeLyUtLKAHlqBwCgzxAAUA0BANUQAFANAQDVEABQDQEA1RAAUA0BANWqBMD/63IAha327M/T8z+N+P7+XjQAPwHKu7+/pwD4A536YVxfX5cLAM0BKI4HwB/ryPf399fX1+fnJwIArUMAQLWuAmB+zDhi1esALKaj0fjw1ClMF9lL2DudBMDvelfPAzDiLY8A7LvdB4D3el69xwEYj8csAgjAvuswAP4AG+pxAKaL08Ox3QfxAGzqhhuPtMyCkpac3RS0YlABoB2IY7rYWXFl+z17FLABMD1tDrMj2jHRxCw1kaPot9tZkf7lh2pQAXBX60Da77bJ08Kms5xbQVrNLvKLqTm2g7wIrUEAmmQv+GkEWABYJ6evsummGL/aD/0S3DEEoEm8o9NOlu8A0QCg53cHAWiS09HJoywV3J19UowFgM3kU6ElHQZgNMiPQXm/ug1sNze8EgaAz0T3t273AVi5vR7q8X+EQe90EoBVfgZWvf5RCOidrgIgQABgd4YTgHuASlYmAH61O3d3d7e3tzc3NyUCYNMD0Hf2DvDx8VE0AACDUeWX4gEGAwEA1RAAUA0BANUQAFAtGoD/LPMHaWkmIkEAAAAASUVORK5CYII=>

[image21]: <data:image/png;base64,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>

[image22]: <data:image/png;base64,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>

[image23]: <data:image/png;base64,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>

[image24]: <data:image/png;base64,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>

[image25]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAQMAAADGCAIAAABZ4XIvAAAObElEQVR4Xu2dS28UxxbH/W2MxDKL8AEi2M4i/gJB4t7sQWaDFCW7sI0CQYTMmg8A4rFEsheRLcuW5ZjEARxFvK8XCCFevnWquk6fenRPT0/3dHXP/7cw3adOVY97zq+rZjw9LB1bjgAYIv/TvH79+tWrVy9fvnzx4sWzZ8+ePn3677///vPPP0+ePHn8+PHBwcESTADDBiYAQNQxYQzA4PjNcuPGjV81169fv3bt2i+//HL16tUrV67ABLAQwAQAiIkm/Pzzzz/99FP3Jpij+9HK8OP3G2rBozU1YONEH2HjD7jxATtkUCYU5RTF6yFHa3bkpih6hI0/2sYH7BCYMB3hUGGkW8LHw5GwaUYaH7BD2jKB8+WumzIds4wwS1+PcKgw0i3h4+FI2DQjjQ/YIa2YwMkGjvh50zDLCLP09WhwqJYoeYQlTfVofMAOadGE6EZtqowQ5piIRLZ6CV5TlPJMOZpM83bDSN7HEm3iYAklmXKoaFrFJm6V2+Fuv2jXhJLItFQZoSinSrwox6MkzWsqH7yo1cssaSqiJM00FQ1YsUnulnTpHe2aEG7UpsoIRTnReBgMIyEyx2wX9Qozq7QWbRdFQkpywiaOTNXEVMnpC4tuQoiX5hHmeJF8IBGX29FI3qEgKOGEKCU5YRNHzEZItBfjZfaadk0w216kHlVGKMqJxqPBiYS9ZKTKdhgJW8vj5YS9ODJVEzOxifGbe0WLJjDRyLRU6ViUE41HgxMJe3HEa5K7Jb14WxLmVCfsxZGpmpiKTSVpvaAVE8bus+vtmsi0VOlYlFMxXpTmIdPMttktilfvVZQZbSqhaISwe0lrlaaieB9py4TG4QfgEebISHl8HAzrNxfg9eKOflRT3os3vDRvlxGJZUS7hCNEExiRGG+S20WRvtAbEwYMn1vGzwDtAxMAIGACAMREE3CnDlgI6pgAwPCoc0c/AMMDJgBAwAQACJgAAAETACBgAgAETACAaMWE8WiJGI39hkqsrY5W1/wgAO3Sgglrq8vaAeVD3IXxaFmWurcLQBe0YIKtbGtEAEwA6dGCCcoAt7SzxZIOZtt239vN+vPqaDwara4uU6txas3s5AEAGqIFE0zBisL2F0sT5gTHhGwcpcSYh/I7ADA7NU34M4YcV1+9RfXKqWIaE5wuWUvZS2r/MQFQjYea/f39PzR7e3u7u7s7Ozvb29tbW1ubm5sbGxu///57xAS3AiOY0hcLGjtPTGOCO43YxRSWRqBpqswJ05ngr4L8Qg9K308oNoF3AWia5k1gFbJ/7aqI1/x+6Vc3gecXCAGapgUTeBFj6zUr4Lx85ZtJ4W6xCf5iCYDGaMWElsj/QAETQNP0yYSjYLYBoCl6ZgIALQETACBgAgAETACAgAkAEDABAAImAEDABAAImAAAARMAIGDCnPC/pLy3cA0MDJgwJ1QNfdt/Dg8PYQJMmAljAp/GngITYMKswITEgQlzIn0T9OfdI8gcmFDHhPWLJ2L3EYgwf9vRiYvrblIc8e1IS327SSFxE+SJDeG0SiZM+7SmwfxNyJmY4EDnV5xabzd5UjYhWvTReBUTpntak6EvJqhcr/DDSNL0wgS/IWiCCQ2aYMLOQkenUWUT8eqm/NhohvybZfSOHcIO6Yxpg6OLtGG7lR+9ARbHBO4yGpunQp/b7MTyU8LPFz13+qnIcrq6QbcrE9wtcYGPX+rjUSZqQi6P0IgGok1nwElHb4LFMUE8reqpGI3yM6o1yC97+ZPDT8mS0CFWPC2Shgm2Pg35RZ3xKjQ7aXy2Yiasr3O6eE3BmeKQE4/eBAtqgixoZ49PunjuRE+12crTUEwaJjhrJSLoGLtIOP0jpeyMmsWsUlKtyUdvgAU1QZazOs9i1+7BBN+EWK5AXNcz+KrimpCdYVnq4rxm83XwNtSEo88OTHDPMz97MMH/xeVVO1qY+tLtXcdzE0yD7uybIDqabv7oVY4+K70wYanhd1G9lWZ+nsVpXgwT5Klcyn71qAlHXNs2LYoYUp0ncQ6584jelpJW6NSLdkLRz8WJ/HHxoaocfSZSNuHYLfoQTpvNBB3JhpRnfugmJIh7fud6thM34bhYBplTxYSeslgmyGv/kv+6o13SN6EKMGEwJnQGTEgcmDAnlAmHgwAmwISZ4BM4APzfbRDABAAImAAAARMAIGACAARMAICACQAQMAEAAiYAQMAEAAiYAAABEwAgYAIABEwAgIAJABAwAQAiXRMePHhw/vz5M2fOnDp1Sv1U2yriJwHQECmaoB7K999/f/bs2du3b6tH8/79e/VTbauIiqtH6XcAYGZSNOHSpUs//vjj58+f+aAGFbl8+bJq9TsAMDPJmaCWQN9884051t27d1dWVtTqSP1U2yaoWuUyyfuqO+8rjPSO+EadWeFv5HG/q6mQimmge5Iz4cKFC7du3VIHun///hcuRgbVql4zcD5Vf1Zs5qu+rBcsQismFIbc3Ug+SJPkTPjqq6/UQ1EHUvOAZ4KKqPjz589Pnz6dd+BvgtQVP5LfC5kVIUwAk0nOBFXx5kBqUeSZoCIqrl5Aq428A5e8UcKWfS6IMcFMGM6XPdpQ/gVgRZkMV7bZcL5HjP7jDGdX5ptt3TDP7xsDlUnOhKnnBFvz6+a7HTMxxJSQ1autTDFn+FsmM4gKPBPcrciuzC8dGHRNciao1wC3b99WB7p3755nwp07d1RctcrXCUeZCuNgauByo/rm6rT/z4gwhYNyIsmDkromxA8HEiI5Ex48eHD27NlPnz6pY6nS//rrr7/88kv102ig4qrV+xObLjN+gaDLeTQSX/5rF0zZjm5wFjI8ZcQyHeqaED8cSIjkTFD88MMPly9fLvp7gmr1O5g6s3VLYji1FqtvZ55gYpkOs5gQHg4kRIomqMfBf2NWrwrUS2T1s+xvzKb2udL0rqjiaH1TUpaj823BhpmSuAl2HPqf3dxdmR8eDiREiiYYzOeO1IvjU6dOqZ+lnzui4hK15V2Bi+o7X7LY5qJMJmJCPk6+OuPdWBo0SJJ0TQBgnsAEAAiYAAABEwAgYAIABEwAgIAJABDpmoD7mME8SdGEV7iPGcydFE3Afcxg/iRngryPOUqn9zFb9Ccn3EHlBytA/0jOBL6PuYhO72M2mAN5KkwwYUIz6JrkTOB71orw71kbz/M+Zo35XOlF74PWE0p9QjPomuRM+MLex1xEp/cxE0YEvk9UhHnPDpFPSjlFw4JuSc6EqecEW/Pr87iP2WlwVWATtAZ8NJshRQEJkpwJfB9zEZ3ex+x2dGyxpe4cLc+GCYmTnAnyPuaQru9jDvuxCsIE0Y/3YELiJGfCUdL3MfvdxKzQ1zkh9mvGgxOp1ysRUjThZbL3MfsiyGG41J2QfJ1gD5dWsUR+zYLgROr1SoQUTTCkdx+zLOwcezB50eeRZbYNnriYxxIg+DULgxOp1ysR0jUBNIOZMLWBokqtlvQVlsGlwQkKqg/lZ4oZOLKbBDBh2OTTo5go80LUVWw2o0FJ9aFimWPxRoI3hacBTBg06/mLEirKvOblQk5Ho0HJFEPFMsWYSYoAE4aOvlRb/Ev2kS5LHY0GXaoOFc3MBYgP3jkwYcg463GuwPiFPBYUVB8qnsnJkbGTACYMGVGU+jKdbedh2hKbQTCn+lAFmTYlHDoNYMKwyYqPyo8+PMtFaNcvFC4PMtWHKsr0VlNpARMAIGACAARMAICACQAQMAEAAiYAQMAEAAiYAAABEwAgYAIARH0TABgSDzX7+/t/aPb29nZ3d3d2dra3t7e2tjY3Nzc2NiImADAwqswJBwcHMAEMHJgAAAETACBgAgAETACAgAkAEO2YoG/oE3fpra0u0818y6trIolZWx3FG3zkVybE7wH0hqow8ngUHwksGO2YQKXvilC93MajAmFqVC1MAFWZkwkTCzIHJoAuaN2EbGFUsjgS9SqWP7HkoGqz/Dy3bHXkJZvd5eUppiswYFo3weyVXprd5tI5wdHEHkT0LzbBJo/N12Fn//C/YNGpaYL/8SWNGLYtE5yqXVvLfMgnhUIT+AGpDZUtd3lM//cBi0TNT+D9OeFT2XMxwcwRy6vjCnOCWKSRN/lU4I8JFpQqc0KiJpiru96YbII3bHROAIvMAExgI3SsyASbpWYD+jebFGiqgAngqC0TvL+sTWWC6Rx988hfyWRv/4xGrEKxCabqxcPKllarWB0Boh0TAOgbMAEAAiYAQMAEAAiYAAABEwAgYAIABEwAgIAJABCNmTAGoC6qrvzQ3PnNcuPGjV81169fv3bt2ps3b6Y24VsApufw8NCY4Dd0wX81/9GcO3fu4cOHNU3gNAAqIk3w2+bIZ82nT58+fvz44cOH9+/fv3v3DiaA+QETACC6MoHv3PIYiAk3V5aWTn634wRWbua7IDk6McEvf5ehmLAkax8mpM78TZBFz0G1NJLxIZhw8uRJ4QJMSJ0OTZBB8zphUCas3Nz57iQvkaQJFNe4ntiwMMaGnIUWaIVhmmAWJw4rN+cWPObCz18usAm6uPVmvmUWUyYx1yeyFT3c3IL23A+TYZogRu4GW/hc7TZAJeZMDjaaX/ZvruhtbpRB0BowoRV4CrAuCBNESdu9PF0H49f/oV+UOwYmtIIsbVvS5XNC1AQU//yACa3glHb2utcE3NV/FoyZIDJlKmiJDk1YGva7qLJw3UrmdY+MhCbITGjQOvM34XgR/rIGekcnJhwXyzCQT1uA3tGVCR7mdQI+gQc6Y4AmHAJQi2Ntgh+dL080qtwfPXr0t+avv/6qYwInANBHonPC27dvpzYBgF7T2B39APQamAAAARMAIGACAARMAICACQAQMAEAor4JAAyJh5r9/f0/NHt7e7u7uzs7O9vb21tbW5ubmxsbGxETABgYVeaEg4MDmAAGDkwAgIAJABAVTfg/x12gW6cDS80AAAAASUVORK5CYII=>

[image26]: <data:image/png;base64,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>

[image27]: <data:image/png;base64,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>

[image28]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAP8AAABVCAIAAAAJ7aUVAAAEyklEQVR4Xu2c7XnbIBRGPU7GyRrdINOkHid75HfSbqDaQkKXewEhWUY0nPOjRS8XhOOjj+SRfRmG4S9AN/wZ+f7+/vr6umA/dAX2Q79E7L8C9MHvmff3d+yHvsB+6JdW7B8MumKu0en/QP51wVm0ZX9qMxO2j1y2fQk2gWo0an9h0j52zSqxBVCNPfa7Nyz6LqqwHDuwJGkfu2aV2AKoxqP2u3fOJluxA0uSpnDL27rm1QJ4HvvtjzZ2o2aITpgJPSoXhTpZxqzNaQuipIpX85KCaKi6YCtH2u83dyBnSM1jc5XIzXxxqi0TFT5Ias5oqHLbjnbBVvbb77FhWF5EycDVGlUgN1PtaGILjsLObJNo6JNMF2zlUfujXSosoWRgqsblHpWn2pbowMMp2ZcNfZLpgq3st1+nI5muPCUDozUytAUuUbktU6wWlGOnKlmMDX2S6YKtHGO/Szyyq5CSgbZGJamC1TLFakEUuyMfZhJbEA194hq+11ZCOcfbL/NySsbaGpm4tp2kJMxvFuJG2YEytAU2sbltS5YxsJE99j+DkjcyWuMlkKheUb4QjigycjepHaneVC67opWwj1bsPxzrzc/gR76os/ix9gOsgv3QL9gP/RKxH6ATIp9qB+gE7Id+wX7oF+yHfsF+6Bfsh37BfugX7Id+wX7oF+yHfsF+6BfshwX9FFgzPMlM7IeFm2e/2uPz8/NJZmI/LDj7h8bA/sp8vL1cLi9vHyK6vl50tJdxKsN97tt+X6+6vB7YD477AbDIft96gpe3aY85oI6hvv36FDAja7D/FG7naGd8eCQcSN/2a+VDfBn2n4Tz//Zv4Oh4X3THXw38ceI2XLX7P3ITtaDt93c+9wnf3G7GgulmKbj8zMtITb6dmva7n6Ajn2P/aTjthHWjc+P20kra//qav2jk7HdzT4r7Q2DezXI5OvDCdIr9usN0Yf95KEGlgaN440bCfnWytuTsF56L5lTu9zwy7/FRsB9CrP1ic95K2L/q5G77pzuhhZXDrAzshxBr/8q53x8fz7b/EOEDsB9CIoKKG+5JQW/8mFWwXyxDruNBsB9CtKB/xW3HopyMatz5TDVmHY+B/dAvp9jvLU/l2A81qGn/EIpu8WXYDzWobP+QPgBkDfZDDerbXwL2Qw2wH/rlZv9nkzzJTOyHhfCc2xZ6rUeA/dAv2A/9gv2wcG2VJ5mJ/bBwHf/m0xr81gs1cPaLXzWbAPtPJnh+PwjEM2hZzAP5I/dn1kpnqAD2Q4S0/dvRz3U2RH379elgRtZg/8kY2U1QDvbPaOVDfBn2n4yRPXbnE3nsP4a2P3imv/73OEhq2j/9rEbyOfafTOyuXdl/9R/4XVpRcvaf8D0OklPs1x2mC/tPZv3cH/r48ZE2M2e/8Fw0p/L7LpZVFHxybDPYDxHW7XeZI6/lbvvNBShc0gFgP0Qosl/05cR8yP7MvAeA/RBh3X6h5v0OJXP6322/nPjePP5QwH6IsG7/JKQjq+V++11NyT52gv3QL6fY7y1P5dgPNahp/xCKbvFl2A81qGz/kD4AZA32Qw3q218C9kMNsB/65cp3OkC3hOfcttBrPQLsh37BfugX7Id+wX7oF+yHfsF+6Bfsh37BfugXaf8/xB1wdV5mi9EAAAAASUVORK5CYII=>

[image29]: <data:image/png;base64,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>