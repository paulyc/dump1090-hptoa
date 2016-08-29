// Part of dump1090, a Mode S message decoder for RTLSDR devices.
//
// mode_s.c: Mode S message decoding.
//
// Copyright (c) 2014-2016 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you may copy, redistribute and/or modify it  
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 2 of the License, or (at your  
// option) any later version.  
//
// This file is distributed in the hope that it will be useful, but  
// WITHOUT ANY WARRANTY; without even the implied warranty of  
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU  
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License  
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// This file incorporates work covered by the following copyright and  
// permission notice:
//
//   Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
//
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are
//   met:
//
//    *  Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//    *  Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
//   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#include "dump1090.h"

/* for PRIX64 */
#include <inttypes.h>

//
// ===================== Mode S detection and decoding  ===================
//
//
//

/* A timestamp that indicates the data is synthetic, created from a
 * multilateration result
 */
#define MAGIC_MLAT_TIMESTAMP 0xFF004D4C4154ULL

//=========================================================================
//
// Given the Downlink Format (DF) of the message, return the message length in bits.
//
// All known DF's 16 or greater are long. All known DF's 15 or less are short. 
// There are lots of unused codes in both category, so we can assume ICAO will stick to 
// these rules, meaning that the most significant bit of the DF indicates the length.
//
int modesMessageLenByType(int type) {
    return (type & 0x10) ? MODES_LONG_MSG_BITS : MODES_SHORT_MSG_BITS ;
}

//
//=========================================================================
//
// In the squawk (identity) field bits are interleaved as follows in
// (message bit 20 to bit 32):
//
// C1-A1-C2-A2-C4-A4-ZERO-B1-D1-B2-D2-B4-D4
//
// So every group of three bits A, B, C, D represent an integer from 0 to 7.
//
// The actual meaning is just 4 octal numbers, but we convert it into a hex 
// number tha happens to represent the four octal numbers.
//
// For more info: http://en.wikipedia.org/wiki/Gillham_code
//
static int decodeID13Field(int ID13Field) {
    int hexGillham = 0;

    if (ID13Field & 0x1000) {hexGillham |= 0x0010;} // Bit 12 = C1
    if (ID13Field & 0x0800) {hexGillham |= 0x1000;} // Bit 11 = A1
    if (ID13Field & 0x0400) {hexGillham |= 0x0020;} // Bit 10 = C2
    if (ID13Field & 0x0200) {hexGillham |= 0x2000;} // Bit  9 = A2
    if (ID13Field & 0x0100) {hexGillham |= 0x0040;} // Bit  8 = C4
    if (ID13Field & 0x0080) {hexGillham |= 0x4000;} // Bit  7 = A4
  //if (ID13Field & 0x0040) {hexGillham |= 0x0800;} // Bit  6 = X  or M 
    if (ID13Field & 0x0020) {hexGillham |= 0x0100;} // Bit  5 = B1 
    if (ID13Field & 0x0010) {hexGillham |= 0x0001;} // Bit  4 = D1 or Q
    if (ID13Field & 0x0008) {hexGillham |= 0x0200;} // Bit  3 = B2
    if (ID13Field & 0x0004) {hexGillham |= 0x0002;} // Bit  2 = D2
    if (ID13Field & 0x0002) {hexGillham |= 0x0400;} // Bit  1 = B4
    if (ID13Field & 0x0001) {hexGillham |= 0x0004;} // Bit  0 = D4

    return (hexGillham);
}

//
//=========================================================================
//
// Decode the 13 bit AC altitude field (in DF 20 and others).
// Returns the altitude, and set 'unit' to either UNIT_METERS or UNIT_FEET.
//
static int decodeAC13Field(int AC13Field, altitude_unit_t *unit) {
    int m_bit  = AC13Field & 0x0040; // set = meters, clear = feet
    int q_bit  = AC13Field & 0x0010; // set = 25 ft encoding, clear = Gillham Mode C encoding

    if (!m_bit) {
        *unit = UNIT_FEET;
        if (q_bit) {
            // N is the 11 bit integer resulting from the removal of bit Q and M
            int n = ((AC13Field & 0x1F80) >> 2) |
                    ((AC13Field & 0x0020) >> 1) |
                     (AC13Field & 0x000F);
            // The final altitude is resulting number multiplied by 25, minus 1000.
            return ((n * 25) - 1000);
        } else {
            // N is an 11 bit Gillham coded altitude
            int n = ModeAToModeC(decodeID13Field(AC13Field));
            if (n < -12) {
                return INVALID_ALTITUDE;
            }

            return (100 * n);
        }
    } else {
        *unit = UNIT_METERS;
        // TODO: Implement altitude when meter unit is selected
        return INVALID_ALTITUDE;
    }
}

//
//=========================================================================
//
// Decode the 12 bit AC altitude field (in DF 17 and others).
//
static int decodeAC12Field(int AC12Field, altitude_unit_t *unit) {
    int q_bit  = AC12Field & 0x10; // Bit 48 = Q

    *unit = UNIT_FEET;
    if (q_bit) {
        /// N is the 11 bit integer resulting from the removal of bit Q at bit 4
        int n = ((AC12Field & 0x0FE0) >> 1) | 
                 (AC12Field & 0x000F);
        // The final altitude is the resulting number multiplied by 25, minus 1000.
        return ((n * 25) - 1000);
    } else {
        // Make N a 13 bit Gillham coded altitude by inserting M=0 at bit 6
        int n = ((AC12Field & 0x0FC0) << 1) | 
                 (AC12Field & 0x003F);
        n = ModeAToModeC(decodeID13Field(n));
        if (n < -12) {
            return INVALID_ALTITUDE;
        }

        return (100 * n);
    }
}

//
//=========================================================================
//
// Decode the 7 bit ground movement field PWL exponential style scale
//
static int decodeMovementField(int movement) {
    int gspeed;

    // Note : movement codes 0,125,126,127 are all invalid, but they are 
    //        trapped for before this function is called.

    if      (movement  > 123) gspeed = 199; // > 175kt
    else if (movement  > 108) gspeed = ((movement - 108)  * 5) + 100;
    else if (movement  >  93) gspeed = ((movement -  93)  * 2) +  70;
    else if (movement  >  38) gspeed = ((movement -  38)     ) +  15;
    else if (movement  >  12) gspeed = ((movement -  11) >> 1) +   2;
    else if (movement  >   8) gspeed = ((movement -   6) >> 2) +   1;
    else                      gspeed = 0;

    return (gspeed);
}

// Correct a decoded native-endian Address Announced field
// (from bits 8-31) if it is affected by the given error
// syndrome. Updates *addr and returns >0 if changed, 0 if
// it was unaffected.
static int correct_aa_field(uint32_t *addr, struct errorinfo *ei) 
{
    int i;
    int addr_errors = 0;

    if (!ei)
        return 0;

    for (i = 0; i < ei->errors; ++i) {
        if (ei->bit[i] >= 8 && ei->bit[i] <= 31) {
            *addr ^= 1 << (31 - ei->bit[i]);
            ++addr_errors;
        }
    }

    return addr_errors;
}

// Score how plausible this ModeS message looks.
// The more positive, the more reliable the message is

// 1000: DF 0/4/5/16/24 with a CRC-derived address matching a known aircraft

// 1800: DF17/18 with good CRC and an address matching a known aircraft
// 1400: DF17/18 with good CRC and an address not matching a known aircraft
//  900: DF17/18 with 1-bit error and an address matching a known aircraft
//  700: DF17/18 with 1-bit error and an address not matching a known aircraft
//  450: DF17/18 with 2-bit error and an address matching a known aircraft
//  350: DF17/18 with 2-bit error and an address not matching a known aircraft

// 1600: DF11 with IID==0, good CRC and an address matching a known aircraft
//  800: DF11 with IID==0, 1-bit error and an address matching a known aircraft
//  750: DF11 with IID==0, good CRC and an address not matching a known aircraft
//  375: DF11 with IID==0, 1-bit error and an address not matching a known aircraft

// 1000: DF11 with IID!=0, good CRC and an address matching a known aircraft
//  500: DF11 with IID!=0, 1-bit error and an address matching a known aircraft

// 1000: DF20/21 with a CRC-derived address matching a known aircraft
//  500: DF20/21 with a CRC-derived address matching a known aircraft (bottom 16 bits only - overlay control in use)

//   -1: message might be valid, but we couldn't validate the CRC against a known ICAO
//   -2: bad message or unrepairable CRC error

int scoreModesMessage(unsigned char *msg, int validbits)
{
    int msgtype, msgbits, crc, iid;
    uint32_t addr;
    struct errorinfo *ei;
    static unsigned char all_zeros[14] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    if (validbits < 56)
        return -2;

    msgtype = msg[0] >> 3; // Downlink Format
    msgbits = modesMessageLenByType(msgtype);

    if (validbits < msgbits)
        return -2;

    if (!memcmp(all_zeros, msg, msgbits/8))
        return -2;

    crc = modesChecksum(msg, msgbits);

    switch (msgtype) {
    case 0: // short air-air surveillance
    case 4: // surveillance, altitude reply
    case 5: // surveillance, altitude reply
    case 16: // long air-air surveillance
    case 24: // Comm-D (ELM)
    case 25: // Comm-D (ELM)
    case 26: // Comm-D (ELM)
    case 27: // Comm-D (ELM)
    case 28: // Comm-D (ELM)
    case 29: // Comm-D (ELM)
    case 30: // Comm-D (ELM)
    case 31: // Comm-D (ELM)
        return icaoFilterTest(crc) ? 1000 : -1;

    case 11: // All-call reply
        iid = crc & 0x7f;
        crc = crc & 0xffff80;
        addr = (msg[1] << 16) | (msg[2] << 8) | (msg[3]);

        ei = modesChecksumDiagnose(crc, msgbits);
        if (!ei)
            return -2; // can't correct errors

        // see crc.c comments: we do not attempt to fix
        // more than single-bit errors, as two-bit
        // errors are ambiguous in DF11.
        if (ei->errors > 1)
            return -2; // can't correct errors

        // fix any errors in the address field
        correct_aa_field(&addr, ei);

        // validate address
        if (iid == 0) {
            if (icaoFilterTest(addr))
                return 1600 / (ei->errors + 1);
            else
                return 750 / (ei->errors + 1);
        } else {
            if (icaoFilterTest(addr))
                return 1000 / (ei->errors + 1);
            else
                return -1;
        }
        
    case 17:   // Extended squitter
    case 18:   // Extended squitter/non-transponder
        ei = modesChecksumDiagnose(crc, msgbits);
        if (!ei)
            return -2; // can't correct errors

        // fix any errors in the address field
        addr = (msg[1] << 16) | (msg[2] << 8) | (msg[3]);
        correct_aa_field(&addr, ei);        

        if (icaoFilterTest(addr))
            return 1800 / (ei->errors+1);
        else
            return 1400 / (ei->errors+1);

    case 20:   // Comm-B, altitude reply
    case 21:   // Comm-B, identity reply
        if (icaoFilterTest(crc))
            return 1000; // Address/Parity

#if 0
        // This doesn't seem useful, as we mistake a lot of CRC errors
        // for overlay control
        if (icaoFilterTestFuzzy(crc))
            return 500;  // Data/Parity
#endif

        return -2;

    default:
        // unknown message type
        return -2;
    }
}

//
//=========================================================================
//
// Decode a raw Mode S message demodulated as a stream of bytes by detectModeS(), 
// and split it into fields populating a modesMessage structure.
//

static void decodeExtendedSquitter(struct modesMessage *mm);
static void decodeCommB(struct modesMessage *mm);
static char *ais_charset = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?";

// return 0 if all OK
//   -1: message might be valid, but we couldn't validate the CRC against a known ICAO
//   -2: bad message or unrepairable CRC error

int decodeModesMessage(struct modesMessage *mm, unsigned char *msg)
{
    // Work on our local copy.
    memcpy(mm->msg, msg, MODES_LONG_MSG_BYTES);
    if (Modes.net_verbatim) {
        // Preserve the original uncorrected copy for later forwarding
        memcpy(mm->verbatim, msg, MODES_LONG_MSG_BYTES);
    }
    msg = mm->msg;

    // Get the message type ASAP as other operations depend on this
    mm->msgtype         = msg[0] >> 3; // Downlink Format
    mm->msgbits         = modesMessageLenByType(mm->msgtype);
    mm->crc             = modesChecksum(msg, mm->msgbits);
    mm->correctedbits   = 0;
    mm->addr            = 0;

    // Do checksum work and set fields that depend on the CRC
    switch (mm->msgtype) {
    case 0: // short air-air surveillance
    case 4: // surveillance, altitude reply
    case 5: // surveillance, altitude reply
    case 16: // long air-air surveillance
    case 24: // Comm-D (ELM)
    case 25: // Comm-D (ELM)
    case 26: // Comm-D (ELM)
    case 27: // Comm-D (ELM)
    case 28: // Comm-D (ELM)
    case 29: // Comm-D (ELM)
    case 30: // Comm-D (ELM)
    case 31: // Comm-D (ELM)
        // These message types use Address/Parity, i.e. our CRC syndrome is the sender's ICAO address.
        // We can't tell if the CRC is correct or not as we don't know the correct address.
        // Accept the message if it appears to be from a previously-seen aircraft
        if (!icaoFilterTest(mm->crc)) {
           return -1;
        }
        mm->source = SOURCE_MODE_S;
        mm->addr = mm->crc;
        break;

    case 11: // All-call reply
        // This message type uses Parity/Interrogator, i.e. our CRC syndrome is CL + IC from the uplink message
        // which we can't see. So we don't know if the CRC is correct or not.
        //
        // however! CL + IC only occupy the lower 7 bits of the CRC. So if we ignore those bits when testing
        // the CRC we can still try to detect/correct errors.

        mm->IID = mm->crc & 0x7f;
        if (mm->crc & 0xffff80) {
            int addr;
            struct errorinfo *ei = modesChecksumDiagnose(mm->crc & 0xffff80, mm->msgbits);
            if (!ei) {
                return -2; // couldn't fix it
            }

            // see crc.c comments: we do not attempt to fix
            // more than single-bit errors, as two-bit
            // errors are ambiguous in DF11.
            if (ei->errors > 1)
                return -2; // can't correct errors

            mm->correctedbits = ei->errors;
            modesChecksumFix(msg, ei);

            // check whether the corrected message looks sensible
            // we are conservative here: only accept corrected messages that
            // match an existing aircraft.
            addr = (msg[1] << 16) | (msg[2] << 8) | (msg[3]); 
            if (!icaoFilterTest(addr)) {
                return -1;
            }
        }
        mm->source = SOURCE_MODE_S_CHECKED;
        break;

    case 17:   // Extended squitter
    case 18: { // Extended squitter/non-transponder
        struct errorinfo *ei;
        int addr1, addr2;

        // These message types use Parity/Interrogator, but are specified to set II=0

        if (mm->crc != 0) {
            ei = modesChecksumDiagnose(mm->crc, mm->msgbits);
            if (!ei) {
                return -2; // couldn't fix it
            }

            addr1 = (msg[1] << 16) | (msg[2] << 8) | (msg[3]);
            mm->correctedbits = ei->errors;
            modesChecksumFix(msg, ei);
            addr2 = (msg[1] << 16) | (msg[2] << 8) | (msg[3]);
        
            // we are conservative here: only accept corrected messages that
            // match an existing aircraft.
            if (addr1 != addr2 && !icaoFilterTest(addr2)) {
                return -1;
            }
        }

        mm->source = SOURCE_ADSB; // TIS-B decoding will override this if needed
        break;
    }

    case 20: // Comm-B, altitude reply
    case 21: // Comm-B, identity reply
        // These message types either use Address/Parity (see DF0 etc)
        // or Data Parity where the requested BDS is also xored into the top byte.
        // So not only do we not know whether the CRC is right, we also don't know if
        // the ICAO is right! Ow.

        // Try an exact match
        if (icaoFilterTest(mm->crc)) {
            // OK.
            mm->source = SOURCE_MODE_S;
            mm->addr = mm->crc;
            break;
        }

        // BDS / overlay control just doesn't work out.

        return -1; // no good

    default:
        // All other message types, we don't know how to handle their CRCs, give up
        return -2;
    }      

    // decode the bulk of the message

    // AA (Address announced)
    if (mm->msgtype == 11 || mm->msgtype == 17 || mm->msgtype == 18) {
        mm->AA = mm->addr = (msg[1] << 16) | (msg[2] << 8) | (msg[3]);
    }

    // AC (Altitude Code)
    if (mm->msgtype == 0 || mm->msgtype == 4 || mm->msgtype == 16 || mm->msgtype == 20) {
        mm->AC = ((msg[2] << 8) | msg[3]) & 0x1FFF;
        if (mm->AC) { // Only attempt to decode if a valid (non zero) altitude is present
            mm->altitude = decodeAC13Field(mm->AC, &mm->altitude_unit);
            if (mm->altitude != INVALID_ALTITUDE)
                mm->altitude_valid = 1;
            mm->altitude_source = ALTITUDE_BARO;
        }
    }

    // AF (DF19 Application Field) not decoded

    // CA (Capability)
    if (mm->msgtype == 11 || mm->msgtype == 17) {
        mm->CA = (msg[0] & 0x07);

        switch (mm->CA) {
        case 0:
            mm->airground = AG_UNCERTAIN;
            break;
        case 4:
            mm->airground = AG_GROUND;
            break;
        case 5:
            mm->airground = AG_AIRBORNE;
            break;
        case 6:
            mm->airground = AG_UNCERTAIN;
            break;
        case 7:
            mm->airground = AG_UNCERTAIN;
            break;
        }
    }

    // CC (Cross-link capability)
    if (mm->msgtype == 0) {
        mm->CC = (msg[0] & 0x02) ? 1 : 0;
    }

    // CF (Control field)
    if (mm->msgtype == 18) {
        mm->CF = msg[0] & 7;
    }

    // DR (Downlink Request)
    if (mm->msgtype == 4 || mm->msgtype == 5 || mm->msgtype == 20 || mm->msgtype == 21) {
        mm->DR = (msg[1] >> 3) & 0x1F;
    }

    // FS (Flight Status)
    if (mm->msgtype == 4 || mm->msgtype == 5 || mm->msgtype == 20 || mm->msgtype == 21) {
        mm->FS = msg[0] & 7;
        mm->alert_valid = 1;
        mm->spi_valid = 1;

        switch (mm->FS) {
        case 0:
            mm->airground = AG_UNCERTAIN;
            break;
        case 1:
            mm->airground = AG_GROUND;
            break;
        case 2:
            mm->airground = AG_UNCERTAIN;
            mm->alert = 1;
            break;
        case 3:
            mm->airground = AG_GROUND;
            mm->alert = 1;
            break;
        case 4:
            mm->airground = AG_UNCERTAIN;
            mm->alert = 1;
            mm->spi = 1;
            break;
        case 5:
            mm->airground = AG_UNCERTAIN;
            mm->spi = 1;
            break;
        default:
            mm->spi_valid = 0;
            mm->alert_valid = 0;
            break;
        }
    }

    // ID (Identity)
    if (mm->msgtype == 5  || mm->msgtype == 21) {
        // Gillham encoded Squawk
        mm->ID = ((msg[2] << 8) | msg[3]) & 0x1FFF;
        if (mm->ID) {
            mm->squawk = decodeID13Field(mm->ID);
            mm->squawk_valid = 1;
        }
    }

    // KE (Control, ELM)
    if (mm->msgtype >= 24 && mm->msgtype <= 31) {
        mm->KE = (msg[0] & 0x10) ? 1 : 0;
    }

    // MB (messsage, Comm-B)
    if (mm->msgtype == 20 || mm->msgtype == 21) {
        memcpy(mm->MB, &msg[4], 7);
        decodeCommB(mm);
    }

    // MD (message, Comm-D)
    if (mm->msgtype >= 24 && mm->msgtype <= 31) {
        memcpy(mm->MD, &msg[1], 10);
    }

    // ME (message, extended squitter)
    if (mm->msgtype == 17 || mm->msgtype == 18) {
        memcpy(mm->ME, &msg[4], 7);
        decodeExtendedSquitter(mm);
    }

    // MV (message, ACAS)
    if (mm->msgtype == 16) {
        memcpy(mm->MV, &msg[4], 7);
    }

    // ND (number of D-segment, Comm-D)
    if (mm->msgtype >= 24 && mm->msgtype <= 31) {
        mm->ND = msg[0] & 0x0F;
    }

    // RI (Reply information, ACAS)
    if (mm->msgtype == 0 || mm->msgtype == 16) {
        mm->RI = ((msg[2] & 0x07) << 1) | ((msg[3] >> 7) & 0x01);
    }

    // SL (Sensitivity level, ACAS)
    if (mm->msgtype == 0 || mm->msgtype == 16) {
        mm->SL = (msg[1] >> 5) & 0x07;
    }

    // UM (Utility Message)
    if (mm->msgtype == 4 || mm->msgtype == 5 || mm->msgtype == 20 || mm->msgtype == 21) {
        mm->UM = ((msg[1] & 0x07) << 3) | ((msg[2] >> 5) & 0x07);
    }

    // VS (Vertical Status)
    if (mm->msgtype == 0 || mm->msgtype == 16) {
        mm->VS = (msg[0] & 0x04) ? 1 : 0;
        if (mm->VS)
            mm->airground = AG_GROUND;
        else
            mm->airground = AG_UNCERTAIN;
    }

    if (!mm->correctedbits && (mm->msgtype == 17 || mm->msgtype == 18 || (mm->msgtype == 11 && mm->IID == 0))) {
        // No CRC errors seen, and either it was an DF17/18 extended squitter
        // or a DF11 acquisition squitter with II = 0. We probably have the right address.

        // We wait until here to do this as we may have needed to decode an ES to note
        // the type of address in DF18 messages.

        // NB this is the only place that adds addresses!
        icaoFilterAdd(mm->addr);
    }

    // MLAT overrides all other sources
    if (mm->remote && mm->timestampMsg == MAGIC_MLAT_TIMESTAMP)
        mm->source = SOURCE_MLAT;

    // all done
    return 0;
}

// Decode BDS2,0 carried in Comm-B or ES
static void decodeBDS20(struct modesMessage *mm)
{
    uint32_t chars1, chars2;
    unsigned char *msg = mm->msg;
    
    chars1 = (msg[5] << 16) | (msg[6] << 8) | (msg[7]);
    chars2 = (msg[8] << 16) | (msg[9] << 8) | (msg[10]);
    
    // A common failure mode seems to be to intermittently send
    // all zeros. Catch that here.
    if (chars1 == 0 && chars2 == 0)
        return;

    mm->callsign_valid = 1;
    
    mm->callsign[3] = ais_charset[chars1 & 0x3F]; chars1 = chars1 >> 6;
    mm->callsign[2] = ais_charset[chars1 & 0x3F]; chars1 = chars1 >> 6;
    mm->callsign[1] = ais_charset[chars1 & 0x3F]; chars1 = chars1 >> 6;
    mm->callsign[0] = ais_charset[chars1 & 0x3F];
    
    mm->callsign[7] = ais_charset[chars2 & 0x3F]; chars2 = chars2 >> 6;
    mm->callsign[6] = ais_charset[chars2 & 0x3F]; chars2 = chars2 >> 6;
    mm->callsign[5] = ais_charset[chars2 & 0x3F]; chars2 = chars2 >> 6;
    mm->callsign[4] = ais_charset[chars2 & 0x3F];
    
    mm->callsign[8] = '\0';
}

static void decodeExtendedSquitter(struct modesMessage *mm)
{    
    unsigned char *msg = mm->msg;
    unsigned char *me = mm->ME;
    int metype = mm->metype = msg[4] >> 3;   // Extended squitter message type
    int mesub  = mm->mesub  = (metype == 29 ? ((msg[4]&6)>>1) : (msg[4]  & 7));   // Extended squitter message subtype

    int check_imf = 0;

    // Check CF on DF18 to work out the format of the ES and whether we need to look for an IMF bit
    if (mm->msgtype == 18) {
        switch (mm->CF) {
        case 0: //   ADS-B ES/NT devices that report the ICAO 24-bit address in the AA field
            break;

        case 1: //   Reserved for ADS-B for ES/NT devices that use other addressing techniques in the AA field
            mm->addr |= MODES_NON_ICAO_ADDRESS;
            break;

        case 2: //   Fine TIS-B message (formats are close enough to DF17 for our purposes)
            mm->source = SOURCE_TISB;
            check_imf = 1;
            break;

        case 3: //   Coarse TIS-B airborne position and velocity.
            // TODO: decode me.
            // For now we only look at the IMF bit.
            mm->source = SOURCE_TISB;
            if (msg[4] & 0x80)
                mm->addr |= MODES_NON_ICAO_ADDRESS;
            return;

        case 5: //   TIS-B messages that relay ADS-B Messages using anonymous 24-bit addresses (format not explicitly defined, but it seems to follow DF17)
            mm->source = SOURCE_TISB;
            mm->addr |= MODES_NON_ICAO_ADDRESS;
            break;

        case 6: //   ADS-B rebroadcast using the same type codes and message formats as defined for DF = 17 ADS-B messages
            check_imf = 1;
            break;

        default:    // All others, we don't know the format.
            mm->addr |= MODES_NON_ICAO_ADDRESS; // assume non-ICAO
            return;
        }
    }

    switch (metype) {
    case 1: case 2: case 3: case 4: {
        // Aircraft Identification and Category
        uint32_t chars1, chars2;

        chars1 = (msg[5] << 16) | (msg[6] << 8) | (msg[7]);
        chars2 = (msg[8] << 16) | (msg[9] << 8) | (msg[10]);

        // A common failure mode seems to be to intermittently send
        // all zeros. Catch that here.
        if (chars1 != 0 || chars2 != 0) {
            mm->callsign_valid = 1;

            mm->callsign[3] = ais_charset[chars1 & 0x3F]; chars1 = chars1 >> 6;
            mm->callsign[2] = ais_charset[chars1 & 0x3F]; chars1 = chars1 >> 6;
            mm->callsign[1] = ais_charset[chars1 & 0x3F]; chars1 = chars1 >> 6;
            mm->callsign[0] = ais_charset[chars1 & 0x3F];
        
            mm->callsign[7] = ais_charset[chars2 & 0x3F]; chars2 = chars2 >> 6;
            mm->callsign[6] = ais_charset[chars2 & 0x3F]; chars2 = chars2 >> 6;
            mm->callsign[5] = ais_charset[chars2 & 0x3F]; chars2 = chars2 >> 6;
            mm->callsign[4] = ais_charset[chars2 & 0x3F];
        
            mm->callsign[8] = '\0';
        }

        mm->category = ((0x0E - metype) << 4) | mesub;
        mm->category_valid = 1;
        break;
    }

    case 19: { // Airborne Velocity Message        
        if (check_imf && (msg[5] & 0x80))
            mm->addr |= MODES_NON_ICAO_ADDRESS;

        if ( (mesub >= 1) && (mesub <= 4) ) {
            int vert_rate = ((msg[8] & 0x07) << 6) | (msg[9] >> 2);
            if (vert_rate) {
                --vert_rate;
                if (msg[8] & 0x08) 
                    {vert_rate = 0 - vert_rate;}
                mm->vert_rate =  vert_rate * 64;
                mm->vert_rate_valid = 1;
            }

            mm->vert_rate_source = (msg[8] & 0x10 ? ALTITUDE_GNSS : ALTITUDE_BARO);
        }

        if ((mesub == 1) || (mesub == 2)) {
            unsigned ew_raw = ((msg[5] & 0x03) << 8) |  msg[6];
            unsigned ns_raw = ((msg[7] & 0x7F) << 3) | (msg[8] >> 5);
            
            if (ew_raw && ns_raw) {
                int ew_vel = (ew_raw - 1) * ((msg[5] & 0x04) ? -1 : 1) * ((mesub == 2) ? 4 : 1);
                int ns_vel = (ns_raw - 1) * ((msg[7] & 0x80) ? -1 : 1) * ((mesub == 2) ? 4 : 1);

                // Compute velocity and angle from the two speed components
                mm->speed = (unsigned) sqrt((ns_vel * ns_vel) + (ew_vel * ew_vel) + 0.5);
                mm->speed_valid = 1;
                
                if (mm->speed) {
                    int heading = (int) (atan2(ew_vel, ns_vel) * 180.0 / M_PI + 0.5);
                    // We don't want negative values but a 0-360 scale
                    if (heading < 0)
                        heading += 360;
                    mm->heading = (unsigned) heading;
                    mm->heading_source = HEADING_TRUE;
                    mm->heading_valid = 1;
                }

                mm->speed_source = SPEED_GROUNDSPEED;
            }
        } else if (mesub == 3 || mesub == 4) {
            unsigned airspeed = ((msg[7] & 0x7f) << 3) | (msg[8] >> 5);
            if (airspeed) {
                --airspeed;
                if (mesub == 4) { // If (supersonic) unit is 4 kts
                    airspeed *= 4;
                }
                mm->speed = airspeed;
                mm->speed_source = (msg[7] & 0x80) ? SPEED_TAS : SPEED_IAS;
                mm->speed_valid = 1;
            }

            if (msg[5] & 0x04) {
                mm->heading = ((((msg[5] & 0x03) << 8) | msg[6]) * 45) >> 7;
                mm->heading_source = HEADING_MAGNETIC;
                mm->heading_valid = 1;
            }
        }

        if (msg[10] & 0x7f) {
            mm->gnss_delta_valid = 1;
            mm->gnss_delta = ((msg[10] & 0x80) ? -25 : 25) * ((msg[10] & 0x7f) - 1);
        }

        break;
    }
        
    case 5: case 6: case 7: case 8: {
        // Ground position
        int movement;

        if (check_imf && (msg[6] & 0x08))
            mm->addr |= MODES_NON_ICAO_ADDRESS;

        mm->airground = AG_GROUND; // definitely.
        mm->cpr_lat  = ((msg[6] & 3) << 15) | (msg[7] << 7) | (msg[8] >> 1);
        mm->cpr_lon = ((msg[8] & 1) << 16) | (msg[9] << 8) | (msg[10]);
        if (mm->msg[6] & 0x04)
            mm->cpr_odd = 1;
        mm->cpr_nucp = (14 - metype);
        mm->cpr_valid = 1;

        movement = ((msg[4] << 4) | (msg[5] >> 4)) & 0x007F;
        if ((movement) && (movement < 125)) {
            mm->speed_valid = 1;
            mm->speed = decodeMovementField(movement);
            mm->speed_source = SPEED_GROUNDSPEED;
        }

        if (msg[5] & 0x08) {
            mm->heading_valid = 1;
            mm->heading_source = HEADING_TRUE;
            mm->heading = ((((msg[5] << 4) | (msg[6] >> 4)) & 0x007F) * 45) >> 4;
        }

        break;
    }

    case 0: // Airborne position, baro altitude only
    case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18: // Airborne position, baro
    case 20: case 21: case 22: { // Airborne position, GNSS altitude (HAE or MSL)
        int AC12Field = ((msg[5] << 4) | (msg[6] >> 4)) & 0x0FFF;

        if (check_imf && (msg[4] & 0x01))
            mm->addr |= MODES_NON_ICAO_ADDRESS;

        if (metype == 0) {
            mm->cpr_nucp = 0;
        } else {
            // Catch some common failure modes and don't mark them as valid
            // (so they won't be used for positioning)

            mm->cpr_lat  = ((msg[6] & 3) << 15) | (msg[7] << 7) | (msg[8] >> 1);
            mm->cpr_lon = ((msg[8] & 1) << 16) | (msg[9] << 8) | (msg[10]);

            if (AC12Field == 0 && mm->cpr_lon == 0 && (mm->cpr_lat & 0x0fff) == 0 && mm->metype == 15) {
                // Seen from at least:
                //   400F3F (Eurocopter ECC155 B1) - Bristow Helicopters
                //   4008F3 (BAE ATP) - Atlantic Airlines
                //   400648 (BAE ATP) - Atlantic Airlines
                // altitude == 0, longitude == 0, type == 15 and zeros in latitude LSB.
                // Can alternate with valid reports having type == 14
                Modes.stats_current.cpr_filtered++;
            } else {
                // Otherwise, assume it's valid.
                mm->cpr_valid = 1;
                if (mm->msg[6] & 0x04)
                    mm->cpr_odd = 1;

                if (metype == 18 || metype == 22)
                    mm->cpr_nucp = 0;
                else if (metype < 18)
                    mm->cpr_nucp = (18 - metype);
                else
                    mm->cpr_nucp = (29 - metype);
            }
        }

        if (AC12Field) {// Only attempt to decode if a valid (non zero) altitude is present
            mm->altitude = decodeAC12Field(AC12Field, &mm->altitude_unit);
            if (mm->altitude != INVALID_ALTITUDE) {
                mm->altitude_valid = 1;
            }

            mm->altitude_source = (metype == 20 || metype == 21 || metype == 22) ? ALTITUDE_GNSS : ALTITUDE_BARO;
        }

        break;
    }

    case 23: { // Test message
        if (mesub == 7) {               // (see 1090-WP-15-20)
            int ID13Field = (((msg[5] << 8) | msg[6]) & 0xFFF1)>>3;
            if (ID13Field) {
                mm->squawk_valid = 1;
                mm->squawk   = decodeID13Field(ID13Field);
            }
        }
        break;
    }

    case 24: // Reserved for Surface System Status
        break;

    case 28: { // Extended Squitter Aircraft Status
        if (mesub == 1) {      // Emergency status squawk field
            int ID13Field = (((msg[5] << 8) | msg[6]) & 0x1FFF);
            if (ID13Field) {
                mm->squawk_valid = 1;
                mm->squawk   = decodeID13Field(ID13Field);
            }

            if (check_imf && (msg[10] & 0x01))
                mm->addr |= MODES_NON_ICAO_ADDRESS;
        }
        break;
    }

    case 29: // Aircraft Trajectory Intent
        if (mesub == 1) { // Target state and status, V2
            mm->tss.valid = 1;
            mm->tss.sil_type = (me[0] & 0x01) ? SIL_PER_SAMPLE : SIL_PER_HOUR;
            mm->tss.altitude_type = (me[1] & 0x80) ? TSS_ALTITUDE_FMS : TSS_ALTITUDE_MCP;

            unsigned alt_bits = ((me[1] << 4) | (me[2] >> 4)) & 0x7FF;
            if (alt_bits == 0) {
                mm->tss.altitude_valid = 0;
            } else {
                mm->tss.altitude_valid = 1;
                mm->tss.altitude = (alt_bits - 1) * 32;
            }

            unsigned baro_bits = ((me[2] << 5) | (me[3] >> 3)) & 0x1FF;
            if (baro_bits == 0) {
                mm->tss.baro_valid = 0;
            } else {
                mm->tss.baro_valid = 1;
                mm->tss.baro = 800.0 + (baro_bits - 1) * 0.8;
            }

            mm->tss.heading_valid = (me[3] & 0x04) != 0;
            if (mm->tss.heading_valid) {
                // two's complement -180..+180, which is conveniently
                // also the same as unsigned 0..360
                unsigned heading_bits = ((me[3] << 7) | (me[4] >> 1)) & 0x1ff;
                mm->tss.heading = heading_bits * 180 / 256;
            }

            mm->tss.nac_p = ((me[4] << 3) | (me[5] >> 5)) & 0x0f;
            mm->tss.nic_baro = (me[5] & 0x10) ? 1 : 0;
            mm->tss.sil = (me[5] >> 2) & 0x03;
            mm->tss.mode_valid = (me[5] & 0x02) ? 1 : 0;
            if (mm->tss.mode_valid) {
                mm->tss.mode_autopilot = (me[5] & 0x01) != 0;
                mm->tss.mode_vnav = (me[6] & 0x80) != 0;
                mm->tss.mode_alt_hold = (me[6] & 0x40) != 0;
                mm->tss.mode_approach = (me[6] & 0x10) != 0;
            }

            mm->tss.acas_operational = (me[6] & 0x08) != 0;
        }
        break;

    case 30: // Aircraft Operational Coordination
        break;

    case 31: // Aircraft Operational Status
        if (check_imf && (msg[10] & 0x01))
            mm->addr |= MODES_NON_ICAO_ADDRESS;

        if (mm->mesub == 0 || mm->mesub == 1) {
            mm->opstatus.valid = 1;
            mm->opstatus.version = (me[5] >> 5) & 0x07;

            switch (mm->opstatus.version) {
            case 0:
                break;

            case 1:
                if ((me[3] & 0xC0) == 0) {
                    mm->opstatus.om_acas_ra = (me[3] & 0x20) != 0;
                    mm->opstatus.om_ident = (me[3] & 0x10) != 0;
                    mm->opstatus.om_atc = (me[3] & 0x08) != 0;
                }

                if (mm->mesub == 0 && (me[1] & 0xCC) == 0) {
                    // airborne
                    mm->opstatus.cc_acas = (me[1] & 0x20) == 0;
                    mm->opstatus.cc_cdti = (me[1] & 0x10) != 0;
                    mm->opstatus.cc_arv = (me[1] & 0x02) != 0;
                    mm->opstatus.cc_ts = (me[1] & 0x01) != 0;
                    mm->opstatus.cc_tc = (me[2] >> 6) & 0x03;
                } else if (mm->mesub == 1 && (me[1] & 0xCC) == 0) {
                    // surface
                    mm->opstatus.cc_poa = (me[1] & 0x20) != 0;
                    mm->opstatus.cc_cdti = (me[1] & 0x10) != 0;
                    mm->opstatus.cc_b2_low = (me[1] & 0x02) != 0;
                    mm->opstatus.cc_lw_valid = 1;
                    mm->opstatus.cc_lw = me[2] & 0x0F;
                }

                mm->opstatus.nic_supp_a = (me[5] & 0x10) ? 1 : 0;
                mm->opstatus.nac_p = me[5] & 0x0F;
                mm->opstatus.sil = (me[6] >> 4) & 0x03;
                if (mm->mesub == 0) {
                    mm->opstatus.nic_baro = (me[6] & 0x08) ? 1 : 0;
                } else {
                    mm->opstatus.track_angle = (me[6] & 0x08) ? ANGLE_TRACK : ANGLE_HEADING;
                }
                mm->opstatus.hrd = (me[6] & 0x04) ? HEADING_MAGNETIC : HEADING_TRUE;
                break;

            case 2:
            default:
                if ((me[3] & 0xC0) == 0) {
                    mm->opstatus.om_acas_ra = (me[3] & 0x20) != 0;
                    mm->opstatus.om_ident = (me[3] & 0x10) != 0;
                    mm->opstatus.om_atc = (me[3] & 0x08) != 0;
                    mm->opstatus.om_saf = (me[3] & 0x04) != 0;
                    mm->opstatus.om_sda = (me[3] & 0x03);
                }

                if (mm->mesub == 0 && (me[1] & 0xCC) == 0) {
                    // airborne
                    mm->opstatus.cc_acas = (me[1] & 0x20) != 0;
                    mm->opstatus.cc_1090_in = (me[1] & 0x10) != 0;
                    mm->opstatus.cc_arv = (me[1] & 0x02) != 0;
                    mm->opstatus.cc_ts = (me[1] & 0x01) != 0;
                    mm->opstatus.cc_tc = (me[2] >> 6) & 0x03;
                    mm->opstatus.cc_uat_in = (me[2] & 0x20) != 0;
                } else if (mm->mesub == 1 && (me[1] & 0xCC) == 0) {
                    // surface
                    mm->opstatus.cc_poa = (me[1] & 0x20) != 0;
                    mm->opstatus.cc_1090_in = (me[1] & 0x10) != 0;
                    mm->opstatus.cc_b2_low = (me[1] & 0x02) != 0;
                    mm->opstatus.cc_uat_in = (me[1] & 0x01) != 0;
                    mm->opstatus.cc_nac_v = (me[2] >> 5) & 0x07;
                    mm->opstatus.cc_nic_supp_c = (me[2] & 0x10) ? 1 : 0;
                    mm->opstatus.cc_lw_valid = 1;
                    mm->opstatus.cc_lw = me[2] & 0x0F;
                    mm->opstatus.cc_antenna_offset = me[3];
                }

                mm->opstatus.nic_supp_a = (me[5] & 0x10) ? 1 : 0;
                mm->opstatus.nac_p = me[5] & 0x0F;
                mm->opstatus.sil = (me[6] >> 4) & 0x03;
                if (mm->mesub == 0) {
                    mm->opstatus.gva = (me[6] >> 6) & 0x03;
                    mm->opstatus.nic_baro = (me[6] & 0x08) ? 1 : 0;
                } else {
                    mm->opstatus.track_angle = (me[6] & 0x08) ? ANGLE_TRACK : ANGLE_HEADING;
                }
                mm->opstatus.hrd = (me[6] & 0x04) ? HEADING_MAGNETIC : HEADING_TRUE;
                mm->opstatus.sil_type = (me[6] & 0x02) ? SIL_PER_SAMPLE : SIL_PER_HOUR;
                break;
            }
        }
        break;

    default: 
        break;
    }
}

static void decodeCommB(struct modesMessage *mm)
{    
    unsigned char *msg = mm->msg;

    // This is a bit hairy as we don't know what the requested register was
    if (msg[4] == 0x20) { // BDS 2,0 Aircraft Identification
        decodeBDS20(mm);
    }
}

static const char *df_names[33] = {
    /* 0 */ "Short Air-Air Surveillance",
    /* 1 */ NULL,
    /* 2 */ NULL,
    /* 3 */ NULL,
    /* 4 */ "Survelliance, Altitude Reply",
    /* 5 */ "Survelliance, Identity Reply",
    /* 6 */ NULL,
    /* 7 */ NULL,
    /* 8 */ NULL,
    /* 9 */ NULL,
    /* 10 */ NULL,
    /* 11 */ "All Call Reply",
    /* 12 */ NULL,
    /* 13 */ NULL,
    /* 14 */ NULL,
    /* 15 */ NULL,
    /* 16 */ "Long Air-Air ACAS",
    /* 17 */ "Extended Squitter",
    /* 18 */ "Extended Squitter (Non-Transponder)",
    /* 19 */ "Extended Squitter (Military)",
    /* 20 */ "Comm-B, Altitude Reply",
    /* 21 */ "Comm-B, Identity Reply",
    /* 22 */ "Military Use",
    /* 23 */ NULL,
    /* 24 */ "Comm-D Extended Length Message",
    /* 25 */ "Comm-D Extended Length Message",
    /* 26 */ "Comm-D Extended Length Message",
    /* 27 */ "Comm-D Extended Length Message",
    /* 28 */ "Comm-D Extended Length Message",
    /* 29 */ "Comm-D Extended Length Message",
    /* 30 */ "Comm-D Extended Length Message",
    /* 31 */ "Comm-D Extended Length Message",
    /* 32 */ "Mode A/C Reply",
};

static const char *df_to_string(unsigned df) {
    if (df > 32)
        return "out of range";
    if (!df_names[df])
        return "reserved";
    return df_names[df];
}

static const char *altitude_unit_to_string(altitude_unit_t unit) {
    switch (unit) {
    case UNIT_FEET:
        return "ft";
    case UNIT_METERS:
        return "m";
    default:
        return "(unknown altitude unit)";
    }
}

static const char *altitude_source_to_string(altitude_source_t source) {
    switch (source) {
    case ALTITUDE_BARO:
        return "barometric";
    case ALTITUDE_GNSS:
        return "GNSS";
    default:
        return "(unknown altitude source)";
    }
}

static const char *airground_to_string(airground_t airground) {
    switch (airground) {
    case AG_GROUND:
        return "ground";
    case AG_AIRBORNE:
        return "airborne";
    case AG_INVALID:
        return "invalid";
    case AG_UNCERTAIN:
        return "airborne?";
    default:
        return "(unknown airground state)";
    }
}

static const char *speed_source_to_string(speed_source_t speed) {
    switch (speed) {
    case SPEED_GROUNDSPEED:
        return "groundspeed";
    case SPEED_IAS:
        return "IAS";
    case SPEED_TAS:
        return "TAS";
    default:
        return "(unknown speed type)";
    }
}

static void print_hex_bytes(unsigned char *data, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        printf("%02X", (unsigned)data[i]);
    }
}

static int esTypeHasSubtype(unsigned metype)
{
    if (metype <= 18) {
        return 0;
    }

    if (metype >= 20 && metype <= 22) {
        return 0;
    }

    return 1;
}

static const char *esTypeName(unsigned metype, unsigned mesub)
{
    switch (metype) {
    case 0:
        return "No position information (airborne or surface)";

    case 1: case 2: case 3: case 4:
        return "Aircraft identification and category";

    case 5: case 6: case 7: case 8:
        return "Surface position";

    case 9: case 10: case 11: case 12:
    case 13: case 14: case 15: case 16:
    case 17: case 18:
        return "Airborne position (barometric altitude)";

    case 19:
        switch (mesub) {
        case 1:
            return "Airborne velocity over ground, subsonic";
        case 2:
            return "Airborne velocity over ground, supersonic";
        case 3:
            return "Airspeed and heading, subsonic";
        case 4:
            return "Airspeed and heading, supersonic";
        default:
            return "Unknown";
        }

    case 20: case 21: case 22:
        return "Airborne position (GNSS altitude)";

    case 23:
        switch (mesub) {
        case 0:
            return "Test message";
        case 7:
            return "National use / 1090-WP-15-20 Mode A squawk";
        default:
            return "Unknown";
        }

    case 24:
        return "Reserved for surface system status";

    case 27:
        return "Reserved for trajectory change";

    case 28:
        switch (mesub) {
        case 1:
            return "Emergency/priority status";
        case 2:
            return "ACAS RA broadcast";
        default:
            return "Unknown";
        }

    case 29:
        switch (mesub) {
        case 1:
            return "Target state and status";
        default:
            return "Unknown";
        }

    case 31: // Aircraft Operational Status
        switch (mesub) {
        case 0:
            return "Aircraft operational status (airborne)";
        case 1:
            return "Aircraft operational status (surface)";
        default:
            return "Unknown";
        }

    default:
        return "Unknown";
    }
}

void displayModesMessage(struct modesMessage *mm) {
    int j;

    // Handle only addresses mode first.
    if (Modes.onlyaddr) {
        printf("%06x\n", mm->addr);
        return;         // Enough for --onlyaddr mode
    }

    // Show the raw message.
    if (Modes.mlat && mm->timestampMsg) {
        printf("@%012" PRIX64, mm->timestampMsg);
    } else
        printf("*");

    for (j = 0; j < mm->msgbits/8; j++) printf("%02x", mm->msg[j]);
    printf(";\n");

    if (Modes.raw) {
        fflush(stdout); // Provide data to the reader ASAP
        return;         // Enough for --raw mode
    }

    if (mm->msgtype < 32)
        printf("CRC: %06x\n", mm->crc);

    if (mm->correctedbits != 0)
        printf("No. of bit errors fixed: %d\n", mm->correctedbits);

    if (mm->signalLevel > 0)
        printf("RSSI: %.1f dBFS\n", 10 * log10(mm->signalLevel));

    if (mm->score)
        printf("Score: %d\n", mm->score);

    if (mm->timestampMsg) {
        if (mm->timestampMsg == MAGIC_MLAT_TIMESTAMP)
            printf("This is a synthetic MLAT message.\n");
        else
            printf("Time: %.2fus\n", mm->timestampMsg / 12.0);
    }

    switch (mm->msgtype) {
    case 0:
        printf("DF:0 addr:%06X VS:%u CC:%u SL:%u RI:%u AC:%u\n",
               mm->addr, mm->VS, mm->CC, mm->SL, mm->RI, mm->AC);
        break;

    case 4:
        printf("DF:4 addr:%06X FS:%u DR:%u UM:%u AC:%u\n",
               mm->addr, mm->FS, mm->DR, mm->UM, mm->AC);
        break;

    case 5:
        printf("DF:5 addr:%06X FS:%u DR:%u UM:%u ID:%u\n",
               mm->addr, mm->FS, mm->DR, mm->UM, mm->ID);
        break;

    case 11:
        printf("DF:11 AA:%06X IID:%u CA:%u\n",
               mm->AA, mm->IID, mm->CA);
        break;

    case 16:
        printf("DF:16 addr:%06x VS:%u SL:%u RI:%u AC:%u MV:",
               mm->addr, mm->VS, mm->SL, mm->RI, mm->AC);
        print_hex_bytes(mm->MV, sizeof(mm->MV));
        printf("\n");
        break;

    case 17:
        printf("DF:17 AA:%06X CA:%u ME:",
               mm->AA, mm->CA);
        print_hex_bytes(mm->ME, sizeof(mm->ME));
        printf("\n");
        break;

    case 18:
        printf("DF:18 AA:%06X CF:%u ME:",
               mm->AA, mm->CF);
        print_hex_bytes(mm->ME, sizeof(mm->ME));
        printf("\n");
        break;

    case 20:
        printf("DF:20 addr:%06X FS:%u DR:%u UM:%u AC:%u MB:",
               mm->addr, mm->FS, mm->DR, mm->UM, mm->AC);
        print_hex_bytes(mm->MB, sizeof(mm->MB));
        printf("\n");
        break;

    case 21:
        printf("DF:21 addr:%06x FS:%u DR:%u UM:%u ID:%u MB:",
               mm->addr, mm->FS, mm->DR, mm->UM, mm->ID);
        print_hex_bytes(mm->MB, sizeof(mm->MB));
        printf("\n");
        break;

    case 24:
    case 25:
    case 26:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
        printf("DF:24 addr:%06x KE:%u ND:%u MD:",
               mm->addr, mm->KE, mm->ND);
        print_hex_bytes(mm->MD, sizeof(mm->MD));
        printf("\n");
        break;
    }

    printf(" %s", df_to_string(mm->msgtype));
    if (mm->msgtype == 17 || mm->msgtype == 18) {
        if (esTypeHasSubtype(mm->metype)) {
            printf(" %s (%u/%u)",
                   esTypeName(mm->metype, mm->mesub),
                   mm->metype,
                   mm->mesub);
        } else {
            printf(" %s (%u)",
                   esTypeName(mm->metype, mm->mesub),
                   mm->metype);
        }
    }
    printf("\n");

    if (mm->addr & MODES_NON_ICAO_ADDRESS) {
        printf("  Other Address: %06X\n", mm->addr);
    } else {
        printf("  ICAO Address:  %06X\n", mm->addr);
    }

    if (mm->airground != AG_INVALID) {
        printf("  Air/Ground:    %s\n",
               airground_to_string(mm->airground));
    }

    if (mm->altitude_valid) {
        printf("  Altitude:      %d %s %s\n",
               mm->altitude,
               altitude_unit_to_string(mm->altitude_unit),
               altitude_source_to_string(mm->altitude_source));
    }

    if (mm->gnss_delta_valid) {
        printf("  GNSS delta:    %d ft\n",
               mm->gnss_delta);
    }

    if (mm->heading_valid) {
        printf("  Heading:       %u\n", mm->heading);
    }

    if (mm->speed_valid) {
        printf("  Speed:         %u kt %s\n",
               mm->speed,
               speed_source_to_string(mm->speed_source));
    }

    if (mm->vert_rate_valid) {
        printf("  Vertical rate: %d ft/min %s\n",
               mm->vert_rate,
               altitude_source_to_string(mm->vert_rate_source));
    }

    if (mm->squawk_valid) {
        printf("  Squawk:        %04x\n",
               mm->squawk);
    }

    if (mm->callsign_valid) {
        printf("  Ident:         %s\n",
               mm->callsign);
    }

    if (mm->category_valid) {
        printf("  Category:      %02X\n",
               mm->category);
    }

    if (mm->msgtype == 17 || mm->msgtype == 18) {
    }

    if (mm->cpr_valid) {
        printf("  CPR odd flag:  %s\n"
               "  CPR NUCp/NIC:  %u\n",
               mm->cpr_odd ? "odd" : "even",
               mm->cpr_nucp);

        if (mm->cpr_decoded) {
            printf("  CPR latitude:  %.5f (%u)\n"
                   "  CPR longitude: %.5f (%u)\n"
                   "  CPR decoding:  %s\n",
                   mm->decoded_lat,
                   mm->cpr_lat,
                   mm->decoded_lon,
                   mm->cpr_lon,
                   mm->cpr_relative ? "local" : "global");
        } else {
            printf("  CPR latitude:  (%u)\n"
                   "  CPR longitude: (%u)\n"
                   "  CPR decoding:  none\n",
                   mm->cpr_lat,
                   mm->cpr_lon);
        }
    }

    if (mm->opstatus.valid) {
        printf("  Aircraft Operational Status:\n");
        printf("    Version:            %d\n", mm->opstatus.version);

        printf("    Capability classes: ");
        if (mm->opstatus.cc_acas) printf("ACAS ");
        if (mm->opstatus.cc_cdti) printf("CDTI ");
        if (mm->opstatus.cc_1090_in) printf("1090IN ");
        if (mm->opstatus.cc_arv) printf("ARV ");
        if (mm->opstatus.cc_ts) printf("TS ");
        if (mm->opstatus.cc_tc) printf("TC=%d ", mm->opstatus.cc_tc);
        if (mm->opstatus.cc_uat_in) printf("UATIN ");
        if (mm->opstatus.cc_poa) printf("POA ");
        if (mm->opstatus.cc_b2_low) printf("B2-LOW ");
        if (mm->opstatus.cc_nac_v) printf("NACv=%d ", mm->opstatus.cc_nac_v);
        if (mm->opstatus.cc_nic_supp_c) printf("NIC-C=1 ");
        if (mm->opstatus.cc_lw_valid) printf("L/W=%d ", mm->opstatus.cc_lw);
        if (mm->opstatus.cc_antenna_offset) printf("GPS-OFFSET=%d ", mm->opstatus.cc_antenna_offset);
        printf("\n");

        printf("    Operational modes:  ");
        if (mm->opstatus.om_acas_ra) printf("ACASRA ");
        if (mm->opstatus.om_ident)   printf("IDENT ");
        if (mm->opstatus.om_atc)     printf("ATC ");
        if (mm->opstatus.om_saf)     printf("SAF ");
        if (mm->opstatus.om_sda)     printf("SDA=%d ", mm->opstatus.om_sda);
        printf("\n");

        if (mm->opstatus.nic_supp_a) printf("    NIC-A:              %d\n", mm->opstatus.nic_supp_a);
        if (mm->opstatus.nac_p)      printf("    NACp:               %d\n", mm->opstatus.nac_p);
        if (mm->opstatus.gva)        printf("    GVA:                %d\n", mm->opstatus.gva);
        if (mm->opstatus.sil)        printf("    SIL:                %d (%s)\n", mm->opstatus.sil, (mm->opstatus.sil_type == SIL_PER_HOUR ? "per hour" : "per sample"));
        if (mm->opstatus.nic_baro)   printf("    NICbaro:            %d\n", mm->opstatus.nic_baro);

        if (mm->mesub == 1)
            printf("    Heading type:      %s\n", (mm->opstatus.track_angle == ANGLE_HEADING ? "heading" : "track angle"));
        printf("    Heading reference:  %s\n", (mm->opstatus.hrd == HEADING_TRUE ? "true north" : "magnetic north"));
    }

    if (mm->tss.valid) {
        printf("  Target State and Status:\n");
        if (mm->tss.altitude_valid)
            printf("    Target altitude:   %s, %d ft\n", (mm->tss.altitude_type == TSS_ALTITUDE_MCP ? "MCP" : "FMS"), mm->tss.altitude);
        if (mm->tss.baro_valid)
            printf("    Altimeter setting: %.1f millibars\n", mm->tss.baro);
        if (mm->tss.heading_valid)
            printf("    Target heading:    %d\n", mm->tss.heading);
        if (mm->tss.mode_valid) {
            printf("    Active modes:      ");
            if (mm->tss.mode_autopilot) printf("autopilot ");
            if (mm->tss.mode_vnav) printf("VNAV ");
            if (mm->tss.mode_alt_hold) printf("altitude-hold ");
            if (mm->tss.mode_approach) printf("approach ");
            printf("\n");
        }
        printf("    ACAS:              %s\n", mm->tss.acas_operational ? "operational" : "NOT operational");
        printf("    NACp:              %d\n", mm->tss.nac_p);
        printf("    NICbaro:           %d\n", mm->tss.nic_baro);
        printf("    SIL:               %d (%s)\n", mm->tss.sil, (mm->opstatus.sil_type == SIL_PER_HOUR ? "per hour" : "per sample"));
    }

    printf("\n");
    fflush(stdout);
}

//
//=========================================================================
//
// When a new message is available, because it was decoded from the RTL device, 
// file, or received in the TCP input port, or any other way we can receive a 
// decoded message, we call this function in order to use the message.
//
// Basically this function passes a raw message to the upper layers for further
// processing and visualization
//
void useModesMessage(struct modesMessage *mm) {
    struct aircraft *a;

    ++Modes.stats_current.messages_total;

    // Track aircraft state
    a = trackUpdateFromMessage(mm);

    // In non-interactive non-quiet mode, display messages on standard output
    if (!Modes.interactive && !Modes.quiet && (!Modes.show_only || mm->addr == Modes.show_only)) {
        displayModesMessage(mm);
    }

    // Feed output clients.
    // If in --net-verbatim mode, do this for all messages.
    // Otherwise, apply a sanity-check filter and only
    // forward messages when we have seen two of them.

    if (Modes.net) {
        if (Modes.net_verbatim || mm->msgtype == 32) {
            // Unconditionally send
            modesQueueOutput(mm, a);
        } else if (a->messages > 1) {
            // If this is the second message, and we
            // squelched the first message, then re-emit the
            // first message now.
            if (!Modes.net_verbatim && a->messages == 2) {
                modesQueueOutput(&a->first_message, a);
            }
            modesQueueOutput(mm, a);
        }
    }
}

//
// ===================== Mode S detection and decoding  ===================
//
