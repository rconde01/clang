//===--- FormatTokenLexer.cpp - Lex FormatTokens -------------*- C++ ----*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file implements FormatTokenLexer, which tokenizes a source file
/// into a FormatToken stream suitable for ClangFormat.
///
//===----------------------------------------------------------------------===//

#include "FormatTokenLexer.h"
#include "FormatToken.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/CPPFormat/Format.h"
#include "llvm/Support/Regex.h"

namespace clang
{
namespace format
{

FormatTokenLexer::FormatTokenLexer(const SourceManager & SourceMgr,
                                   FileID                ID,
                                   const FormatStyle &   Style,
                                   encoding::Encoding    Encoding)
: FormatTok(nullptr),
  IsFirstToken(true),
  StateStack({LexerState::NORMAL}),
  Column(0),
  TrailingWhitespace(0),
  SourceMgr(SourceMgr),
  ID(ID),
  Style(Style),
  IdentTable(getFormattingLangOpts(Style)),
  Keywords(IdentTable),
  Encoding(Encoding),
  FirstInLineIndex(0),
  FormattingDisabled(false),
  MacroBlockBeginRegex(Style.MacroBlockBegin),
  MacroBlockEndRegex(Style.MacroBlockEnd)
{
   Lex.reset(new Lexer(
       ID, SourceMgr.getBuffer(ID), SourceMgr, getFormattingLangOpts(Style)));
   Lex->SetKeepWhitespaceMode(true);

   for(const std::string & ForEachMacro : Style.ForEachMacros)
      ForEachMacros.push_back(&IdentTable.get(ForEachMacro));
   std::sort(ForEachMacros.begin(), ForEachMacros.end());
}

ArrayRef<FormatToken *>
FormatTokenLexer::lex()
{
   assert(Tokens.empty());
   assert(FirstInLineIndex == 0);
   do
   {
      Tokens.push_back(getNextToken());
      tryMergePreviousTokens();
      if(Tokens.back()->UserNewlinesBefore > 0 || Tokens.back()->IsMultiline)
         FirstInLineIndex = Tokens.size() - 1;
   } while(Tokens.back()->Tok.isNot(tok::eof));
   return Tokens;
}

void
FormatTokenLexer::tryMergePreviousTokens()
{
   if(tryMerge_TMacro())
      return;
   if(tryMergeConflictMarkers())
      return;
   if(tryMergeLessLess())
      return;
}

bool
FormatTokenLexer::tryMergeLessLess()
{
   // Merge X,less,less,Y into X,lessless,Y unless X or Y is less.
   if(Tokens.size() < 3)
      return false;

   bool FourthTokenIsLess = false;
   if(Tokens.size() > 3)
      FourthTokenIsLess = (Tokens.end() - 4)[0]->is(tok::less);

   auto First = Tokens.end() - 3;
   if(First[2]->is(tok::less) || First[1]->isNot(tok::less)
      || First[0]->isNot(tok::less) || FourthTokenIsLess)
      return false;

   // Only merge if there currently is no whitespace between the two "<".
   if(First[1]->PrecedingWhitespaceRange.getBegin()
      != First[1]->PrecedingWhitespaceRange.getEnd())
      return false;

   First[0]->Tok.setKind(tok::lessless);
   First[0]->TokenText = "<<";
   First[0]->FirstLineColumnWidth += 1;
   Tokens.erase(Tokens.end() - 2);
   return true;
}

bool
FormatTokenLexer::tryMergeTokens(ArrayRef<tok::TokenKind> Kinds,
                                 TokenType                NewType)
{
   if(Tokens.size() < Kinds.size())
      return false;

   SmallVectorImpl<FormatToken *>::const_iterator First =
       Tokens.end() - Kinds.size();
   if(!First[0]->is(Kinds[0]))
      return false;
   unsigned AddLength = 0;
   for(unsigned i = 1; i < Kinds.size(); ++i)
   {
      if(!First[i]->is(Kinds[i])
         || First[i]->PrecedingWhitespaceRange.getBegin()
                != First[i]->PrecedingWhitespaceRange.getEnd())
         return false;
      AddLength += First[i]->TokenText.size();
   }
   Tokens.resize(Tokens.size() - Kinds.size() + 1);
   First[0]->TokenText = StringRef(First[0]->TokenText.data(),
                                   First[0]->TokenText.size() + AddLength);
   First[0]->FirstLineColumnWidth += AddLength;
   First[0]->Type = NewType;
   return true;
}

bool
FormatTokenLexer::tryMerge_TMacro()
{
   if(Tokens.size() < 4)
      return false;
   FormatToken * Last = Tokens.back();
   if(!Last->is(tok::r_paren))
      return false;

   FormatToken * String = Tokens[Tokens.size() - 2];
   if(!String->is(tok::string_literal) || String->IsMultiline)
      return false;

   if(!Tokens[Tokens.size() - 3]->is(tok::l_paren))
      return false;

   FormatToken * Macro = Tokens[Tokens.size() - 4];
   if(Macro->TokenText != "_T")
      return false;

   const char * Start        = Macro->TokenText.data();
   const char * End          = Last->TokenText.data() + Last->TokenText.size();
   String->TokenText         = StringRef(Start, End - Start);
   String->IsFirst           = Macro->IsFirst;
   String->LastNewlineOffset = Macro->LastNewlineOffset;
   String->PrecedingWhitespaceRange   = Macro->PrecedingWhitespaceRange;
   String->OriginalColumn    = Macro->OriginalColumn;
   String->FirstLineColumnWidth       = encoding::columnWidthWithTabs(
       String->TokenText, String->OriginalColumn, Style.TabWidth, Encoding);
   String->UserNewlinesBefore      = Macro->UserNewlinesBefore;
   String->HasUnescapedNewlineBefore = Macro->HasUnescapedNewlineBefore;

   Tokens.pop_back();
   Tokens.pop_back();
   Tokens.pop_back();
   Tokens.back() = String;
   return true;
}

bool
FormatTokenLexer::tryMergeConflictMarkers()
{
   if(Tokens.back()->UserNewlinesBefore == 0 && Tokens.back()->isNot(tok::eof))
      return false;

   // Conflict lines look like:
   // <marker> <text from the vcs>
   // For example:
   // >>>>>>> /file/in/file/system at revision 1234
   //
   // We merge all tokens in a line that starts with a conflict marker
   // into a single token with a special token type that the unwrapped line
   // parser will use to correctly rebuild the underlying code.

   FileID ID;
   // Get the position of the first token in the line.
   unsigned FirstInLineOffset;
   std::tie(ID, FirstInLineOffset) = SourceMgr.getDecomposedLoc(
       Tokens[FirstInLineIndex]->getStartOfNonWhitespace());
   StringRef Buffer = SourceMgr.getBuffer(ID)->getBuffer();
   // Calculate the offset of the start of the current line.
   auto LineOffset = Buffer.rfind('\n', FirstInLineOffset);
   if(LineOffset == StringRef::npos)
   {
      LineOffset = 0;
   }
   else
   {
      ++LineOffset;
   }

   auto      FirstSpace = Buffer.find_first_of(" \n", LineOffset);
   StringRef LineStart;
   if(FirstSpace == StringRef::npos)
   {
      LineStart = Buffer.substr(LineOffset);
   }
   else
   {
      LineStart = Buffer.substr(LineOffset, FirstSpace - LineOffset);
   }

   TokenType Type = TT_Unknown;
   if(LineStart == "<<<<<<<" || LineStart == ">>>>")
   {
      Type = TT_ConflictStart;
   }
   else if(LineStart == "|||||||" || LineStart == "======="
           || LineStart == "====")
   {
      Type = TT_ConflictAlternative;
   }
   else if(LineStart == ">>>>>>>" || LineStart == "<<<<")
   {
      Type = TT_ConflictEnd;
   }

   if(Type != TT_Unknown)
   {
      FormatToken * Next = Tokens.back();

      Tokens.resize(FirstInLineIndex + 1);
      // We do not need to build a complete token here, as we will skip it
      // during parsing anyway (as we must not touch whitespace around conflict
      // markers).
      Tokens.back()->Type = Type;
      Tokens.back()->Tok.setKind(tok::kw___unknown_anytype);

      Tokens.push_back(Next);
      return true;
   }

   return false;
}

FormatToken *
FormatTokenLexer::getStashedToken()
{
   // Create a synthesized second '>' or '<' token.
   Token     Tok       = FormatTok->Tok;
   StringRef TokenText = FormatTok->TokenText;

   unsigned OriginalColumn = FormatTok->OriginalColumn;
   FormatTok               = new(Allocator.Allocate()) FormatToken;
   FormatTok->Tok          = Tok;
   SourceLocation TokLocation =
       FormatTok->Tok.getLocation().getLocWithOffset(Tok.getLength() - 1);
   FormatTok->Tok.setLocation(TokLocation);
   FormatTok->PrecedingWhitespaceRange = SourceRange(TokLocation, TokLocation);
   FormatTok->TokenText       = TokenText;
   FormatTok->FirstLineColumnWidth     = 1;
   FormatTok->OriginalColumn  = OriginalColumn + 1;

   return FormatTok;
}

FormatToken *
FormatTokenLexer::getNextToken()
{
   if(StateStack.top() == LexerState::TOKEN_STASHED)
   {
      StateStack.pop();
      return getStashedToken();
   }

   FormatTok = new(Allocator.Allocate()) FormatToken;
   readRawToken(*FormatTok);
   SourceLocation WhitespaceStart =
       FormatTok->Tok.getLocation().getLocWithOffset(-TrailingWhitespace);
   FormatTok->IsFirst = IsFirstToken;
   IsFirstToken       = false;

   // Consume and record whitespace until we find a significant token.
   unsigned WhitespaceLength = TrailingWhitespace;
   while(FormatTok->Tok.is(tok::unknown))
   {
      StringRef Text           = FormatTok->TokenText;
      auto      EscapesNewline = [&](int pos) {
         // A '\r' here is just part of '\r\n'. Skip it.
         if(pos >= 0 && Text[pos] == '\r')
            --pos;
         // See whether there is an odd number of '\' before this.
         // FIXME: This is wrong. A '\' followed by a newline is always removed,
         // regardless of whether there is another '\' before it.
         // FIXME: Newlines can also be escaped by a '?' '?' '/' trigraph.
         unsigned count = 0;
         for(; pos >= 0; --pos, ++count)
            if(Text[pos] != '\\')
               break;
         return count & 1;
      };
      // FIXME: This miscounts tok:unknown tokens that are not just
      // whitespace, e.g. a '`' character.
      for(int i = 0, e = Text.size(); i != e; ++i)
      {
         switch(Text[i])
         {
         case '\n':
            ++FormatTok->UserNewlinesBefore;
            FormatTok->HasUnescapedNewlineBefore = !EscapesNewline(i - 1);
            FormatTok->LastNewlineOffset   = WhitespaceLength + i + 1;
            Column                         = 0;
            break;
         case '\r':
            FormatTok->LastNewlineOffset = WhitespaceLength + i + 1;
            Column                       = 0;
            break;
         case '\f':
         case '\v':
            Column = 0;
            break;
         case ' ':
            ++Column;
            break;
         case '\t':
            Column += Style.TabWidth - Column % Style.TabWidth;
            break;
         case '\\':
            if(i + 1 == e || (Text[i + 1] != '\r' && Text[i + 1] != '\n'))
               FormatTok->Type = TT_ImplicitStringLiteral;
            break;
         default:
            FormatTok->Type = TT_ImplicitStringLiteral;
            break;
         }
         if(FormatTok->Type == TT_ImplicitStringLiteral)
            break;
      }

      if(FormatTok->is(TT_ImplicitStringLiteral))
         break;
      WhitespaceLength += FormatTok->Tok.getLength();

      readRawToken(*FormatTok);
   }

   // In case the token starts with escaped newlines, we want to
   // take them into account as whitespace - this pattern is quite frequent
   // in macro definitions.
   // FIXME: Add a more explicit test.
   while(FormatTok->TokenText.size() > 1 && FormatTok->TokenText[0] == '\\'
         && FormatTok->TokenText[1] == '\n')
   {
      ++FormatTok->UserNewlinesBefore;
      WhitespaceLength += 2;
      FormatTok->LastNewlineOffset = 2;
      Column                       = 0;
      FormatTok->TokenText         = FormatTok->TokenText.substr(2);
   }

   FormatTok->PrecedingWhitespaceRange = SourceRange(
       WhitespaceStart, WhitespaceStart.getLocWithOffset(WhitespaceLength));

   FormatTok->OriginalColumn = Column;

   TrailingWhitespace = 0;
   if(FormatTok->Tok.is(tok::comment))
   {
      // FIXME: Add the trimmed whitespace to Column.
      StringRef UntrimmedText = FormatTok->TokenText;
      FormatTok->TokenText    = FormatTok->TokenText.rtrim(" \t\v\f");
      TrailingWhitespace = UntrimmedText.size() - FormatTok->TokenText.size();
   }
   else if(FormatTok->Tok.is(tok::raw_identifier))
   {
      IdentifierInfo & Info = IdentTable.get(FormatTok->TokenText);
      FormatTok->Tok.setIdentifierInfo(&Info);
      FormatTok->Tok.setKind(Info.getTokenID());
   }
   else if(FormatTok->Tok.is(tok::greatergreater))
   {
      FormatTok->Tok.setKind(tok::greater);
      FormatTok->TokenText = FormatTok->TokenText.substr(0, 1);
      ++Column;
      StateStack.push(LexerState::TOKEN_STASHED);
   }
   else if(FormatTok->Tok.is(tok::lessless))
   {
      FormatTok->Tok.setKind(tok::less);
      FormatTok->TokenText = FormatTok->TokenText.substr(0, 1);
      ++Column;
      StateStack.push(LexerState::TOKEN_STASHED);
   }

   // Now FormatTok is the next non-whitespace token.

   StringRef Text            = FormatTok->TokenText;
   size_t    FirstNewlinePos = Text.find('\n');
   if(FirstNewlinePos == StringRef::npos)
   {
      // FIXME: ColumnWidth actually depends on the start column, we need to
      // take this into account when the token is moved.
      FormatTok->FirstLineColumnWidth =
          encoding::columnWidthWithTabs(Text, Column, Style.TabWidth, Encoding);
      Column += FormatTok->FirstLineColumnWidth;
   }
   else
   {
      FormatTok->IsMultiline = true;
      // FIXME: ColumnWidth actually depends on the start column, we need to
      // take this into account when the token is moved.
      FormatTok->FirstLineColumnWidth = encoding::columnWidthWithTabs(
          Text.substr(0, FirstNewlinePos), Column, Style.TabWidth, Encoding);

      // The last line of the token always starts in column 0.
      // Thus, the length can be precomputed even in the presence of tabs.
      FormatTok->LastLineColumnWidth = encoding::columnWidthWithTabs(
          Text.substr(Text.find_last_of('\n') + 1),
          0,
          Style.TabWidth,
          Encoding);
      Column = FormatTok->LastLineColumnWidth;
   }

   if(!(Tokens.size() > 0 && Tokens.back()->Tok.getIdentifierInfo()
        && Tokens.back()->Tok.getIdentifierInfo()->getPPKeywordID()
               == tok::pp_define)
      && std::find(ForEachMacros.begin(),
                   ForEachMacros.end(),
                   FormatTok->Tok.getIdentifierInfo())
             != ForEachMacros.end())
   {
      FormatTok->Type = TT_ForEachMacro;
   }
   else if(FormatTok->is(tok::identifier))
   {
      if(MacroBlockBeginRegex.match(Text))
      {
         FormatTok->Type = TT_MacroBlockBegin;
      }
      else if(MacroBlockEndRegex.match(Text))
      {
         FormatTok->Type = TT_MacroBlockEnd;
      }
   }

   return FormatTok;
}

void
FormatTokenLexer::readRawToken(FormatToken & Tok)
{
   Lex->LexFromRawLexer(Tok.Tok);
   Tok.TokenText = StringRef(SourceMgr.getCharacterData(Tok.Tok.getLocation()),
                             Tok.Tok.getLength());
   // For formatting, treat unterminated string literals like normal string
   // literals.
   if(Tok.is(tok::unknown))
   {
      if(!Tok.TokenText.empty() && Tok.TokenText[0] == '"')
      {
         Tok.Tok.setKind(tok::string_literal);
         Tok.IsUnterminatedLiteral = true;
      }
   }

   if(Tok.is(tok::comment)
      && (Tok.TokenText == "// clang-format on"
          || Tok.TokenText == "/* clang-format on */"))
   {
      FormattingDisabled = false;
   }

   Tok.Finalized = FormattingDisabled;

   if(Tok.is(tok::comment)
      && (Tok.TokenText == "// clang-format off"
          || Tok.TokenText == "/* clang-format off */"))
   {
      FormattingDisabled = true;
   }
}

void
FormatTokenLexer::resetLexer(unsigned Offset)
{
   StringRef Buffer = SourceMgr.getBufferData(ID);
   Lex.reset(new Lexer(SourceMgr.getLocForStartOfFile(ID),
                       getFormattingLangOpts(Style),
                       Buffer.begin(),
                       Buffer.begin() + Offset,
                       Buffer.end()));
   Lex->SetKeepWhitespaceMode(true);
   TrailingWhitespace = 0;
}

}   // namespace format
}   // namespace clang
