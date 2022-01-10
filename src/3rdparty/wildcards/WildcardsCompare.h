// Copyright 2018 IBM Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// http://developforperformance.com/MatchingWildcards_AnImprovedAlgorithmForBigData.html

// Compares two text strings.  Accepts '?' as a single-character wildcard.
// For each '*' wildcard, seeks out a matching sequence of any characters
// beyond it.  Otherwise compares the strings a character at a time.
//
bool FastWildCompare(char *pWild, char *pTame)
{
    char *pWildSequence;  // Points to prospective wild string match after '*'
    char *pTameSequence;  // Points to prospective tame string match
 
    // Find a first wildcard, if one exists, and the beginning of any
    // prospectively matching sequence after it.
    do
    {
        // Check for the end from the start.  Get out fast, if possible.
        if (!*pTame)
        {
            if (*pWild)
            {
                while (*(pWild++) == '*')
                {
                    if (!(*pWild))
                    {
                        return true;   // "ab" matches "ab*".
                    }
                }
 
                return false;          // "abcd" doesn't match "abc".
            }
            else
            {
                return true;           // "abc" matches "abc".
            }
        }
        else if (*pWild == '*')
        {
            // Got wild: set up for the second loop and skip on down there.
            while (*(++pWild) == '*')
            {
                continue;
            }
 
            if (!*pWild)
            {
                return true;           // "abc*" matches "abcd".
            }
 
            // Search for the next prospective match.
            if (*pWild != '?')
            {
                while (*pWild != *pTame)
                {
                    if (!*(++pTame))
                    {
                        return false;  // "a*bc" doesn't match "ab".
                    }
                }
            }
 
            // Keep fallback positions for retry in case of incomplete match.
            pWildSequence = pWild;
            pTameSequence = pTame;
            break;
        }
        else if (*pWild != *pTame && *pWild != '?')
        {
            return false;              // "abc" doesn't match "abd".
        }
 
        ++pWild;                       // Everything's a match, so far.
        ++pTame;
    } while (true);
 
    // Find any further wildcards and any further matching sequences.
    do
    {
        if (*pWild == '*')
        {
            // Got wild again.
            while (*(++pWild) == '*')
            {
                continue;
            }
 
            if (!*pWild)
            {
                return true;           // "ab*c*" matches "abcd".
            }
 
            if (!*pTame)
            {
                return false;          // "*bcd*" doesn't match "abc".
            }
 
            // Search for the next prospective match.
            if (*pWild != '?')
            {
                while (*pWild != *pTame)
                {
                    if (!*(++pTame))
                    {
                        return false;  // "a*b*c" doesn't match "ab".
                    }
                }
            }
 
            // Keep the new fallback positions.
            pWildSequence = pWild;
            pTameSequence = pTame;
        }
        else if (*pWild != *pTame && *pWild != '?')
        {
            // The equivalent portion of the upper loop is really simple.
            if (!*pTame)
            {
                return false;          // "*bcd" doesn't match "abc".
            }
 
            // A fine time for questions.
            while (*pWildSequence == '?')
            {
                ++pWildSequence;
                ++pTameSequence;
            }
 
            pWild = pWildSequence;
 
            // Fall back, but never so far again.
            while (*pWild != *(++pTameSequence))
            {
                if (!*pTameSequence)
                {
                    return false;      // "*a*b" doesn't match "ac".
                }
            }
 
            pTame = pTameSequence;
        }
 
        // Another check for the end, at the end.
        if (!*pTame)
        {
            if (!*pWild)
            {
                return true;           // "*bc" matches "abc".
            }
            else
            {
                return false;          // "*bc" doesn't match "abcd".
            }
        }
 
        ++pWild;                       // Everything's still a match.
        ++pTame;
    } while (true);
}

// Slower but portable version of FastWildCompare().  Performs no direct
// pointer manipulation.  Can work with wide-character text strings.  Use
// only with null-terminated strings.
//
// Compares two text strings.  Accepts '?' as a single-character wildcard.
// For each '*' wildcard, seeks out a matching sequence of any characters
// beyond it.  Otherwise compares the strings a character at a time.
//
bool FastWildComparePortable(const char *strWild, const char *strTame)
{
    int  iWild = 0;     // Index for both tame and wild strings in upper loop
    int  iTame;         // Index for tame string, set going into lower loop
    int  iWildSequence; // Index for prospective match after '*' (wild string)
    int  iTameSequence; // Index for prospective match (tame string)
 
    // Find a first wildcard, if one exists, and the beginning of any
    // prospectively matching sequence after it.
    do
    {
        // Check for the end from the start.  Get out fast, if possible.
        if (!strTame[iWild])
        {
            if (strWild[iWild])
            {
                while (strWild[iWild++] == '*')
                {
                    if (!strWild[iWild])
                    {
                        return true;   // "ab" matches "ab*".
                    }
                }
 
                return false;          // "abcd" doesn't match "abc".
            }
            else
            {
                return true;           // "abc" matches "abc".
            }
        }
        else if (strWild[iWild] == '*')
        {
            // Got wild: set up for the second loop and skip on down there.
            iTame = iWild;
 
            while (strWild[++iWild] == '*')
            {
                continue;
            }
 
            if (!strWild[iWild])
            {
                return true;           // "abc*" matches "abcd".
            }
 
            // Search for the next prospective match.
            if (strWild[iWild] != '?')
            {
                while (strWild[iWild] != strTame[iTame])
                {
                    if (!strTame[++iTame])
                    {
                        return false;  // "a*bc" doesn't match "ab".
                    }
                }
            }
 
            // Keep fallback positions for retry in case of incomplete match.
            iWildSequence = iWild;
            iTameSequence = iTame;
            break;
        }
        else if (strWild[iWild] != strTame[iWild] && strWild[iWild] != '?')
        {
            return false;              // "abc" doesn't match "abd".
        }
 
        ++iWild;                       // Everything's a match, so far.
    } while (true);
 
    // Find any further wildcards and any further matching sequences.
    do
    {
        if (strWild[iWild] == '*')
        {
            // Got wild again.
            while (strWild[++iWild] == '*')
            {
                continue;
            }
 
            if (!strWild[iWild])
            {
                return true;           // "ab*c*" matches "abcd".
            }
 
            if (!strTame[iTame])
            {
                return false;          // "*bcd*" doesn't match "abc".
            }
 
            // Search for the next prospective match.
            if (strWild[iWild] != '?')
            {
                while (strWild[iWild] != strTame[iTame])
                {
                    if (!strTame[++iTame])
                    {
                        return false;  // "a*b*c" doesn't match "ab".
                    }
                }
            }
 
            // Keep the new fallback positions.
            iWildSequence = iWild;
            iTameSequence = iTame;
        }
        else if (strWild[iWild] != strTame[iTame] && strWild[iWild] != '?')
        {
            // The equivalent portion of the upper loop is really simple.
            if (!strTame[iTame])
            {
                return false;          // "*bcd" doesn't match "abc".
            }
 
            // A fine time for questions.
            while (strWild[iWildSequence] == '?')
            {
                ++iWildSequence;
                ++iTameSequence;
            }
 
            iWild = iWildSequence;
 
            // Fall back, but never so far again.
            while (strWild[iWild] != strTame[++iTameSequence])
            {
                if (!strTame[iTameSequence])
                {
                    return false;      // "*a*b" doesn't match "ac".
                }
            }
 
            iTame = iTameSequence;
        }
 
        // Another check for the end, at the end.
        if (!strTame[iTame])
        {
            if (!strWild[iWild])
            {
                return true;           // "*bc" matches "abc".
            }
            else
            {
                return false;          // "*bc" doesn't match "abcd".
            }
        }
 
        ++iWild;                       // Everything's still a match.
        ++iTame;
    } while (true);
}
